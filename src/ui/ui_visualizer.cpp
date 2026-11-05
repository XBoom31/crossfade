#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Real audio capture
//
// The player hands audio to PipeWire/PulseAudio, so the cleanest way to make
// the visualizer actually react to the music is to listen to the default
// sink's monitor source. `parec` is part of pulseaudio-utils and is present on
// essentially every Linux desktop; when it is missing (or capture fails) the
// renderer falls back to a musical synthetic envelope instead of going flat.
// Everything below runs on one detached thread and publishes smoothed,
// already-normalised bands through atomics, so the UI thread only reads.
// ---------------------------------------------------------------------------
namespace
{
constexpr int kBandCount = 64;
constexpr int kWaveCount = 256;
constexpr int kFftSize = 2048;

std::array<std::atomic<float>, kBandCount> g_bands{};
std::array<std::atomic<float>, kWaveCount> g_wave{};
std::atomic<float> g_bass{0.0f};
std::atomic<float> g_mid{0.0f};
std::atomic<float> g_treble{0.0f};
std::atomic<float> g_energy{0.0f};
std::atomic<bool> g_capture_running{false};
std::atomic<bool> g_capture_started{false};
std::atomic<int> g_capture_state{0}; // 0 starting, 1 live, 2 unavailable

std::string run_and_read(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f)
        return {};
    char buf[512];
    std::string out;
    while (fgets(buf, sizeof(buf), f))
        out += buf;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// In-place iterative radix-2 FFT.
void fft(std::vector<std::complex<float>> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const float ang = -2.0f * 3.14159265358979323846f / (float)len;
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void publish_silence()
{
    for (auto &b : g_bands)
        b.store(0.0f, std::memory_order_relaxed);
    for (auto &w : g_wave)
        w.store(0.0f, std::memory_order_relaxed);
    g_bass.store(0.0f, std::memory_order_relaxed);
    g_mid.store(0.0f, std::memory_order_relaxed);
    g_treble.store(0.0f, std::memory_order_relaxed);
    g_energy.store(0.0f, std::memory_order_relaxed);
}

void capture_worker()
{
    const std::string sink = run_and_read("pactl get-default-sink 2>/dev/null");
    if (sink.empty())
    {
        g_capture_state.store(2);
        return;
    }
    const std::string monitor = sink + ".monitor";
    const double sample_rate = 48000.0;
    // 4096 stereo frames per read keeps latency low (about 85 ms) while the
    // FFT still gets a stable 2048-sample window.
    constexpr size_t frames = 4096;
    std::vector<float> pcm(frames * 2);
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
                  "parec --device='%s' --format=float32le --rate=48000 --channels=2 "
                  "--latency-msec=40 2>/dev/null",
                  monitor.c_str());
    FILE *pipe = popen(cmd, "r");
    if (!pipe)
    {
        g_capture_state.store(2);
        return;
    }
    g_capture_state.store(1);
    g_capture_running.store(true);

    std::vector<std::complex<float>> spectrum(kFftSize);
    std::array<float, kBandCount> smooth{};
    std::array<float, kBandCount> peak{};
    float smooth_bass = 0.0f, smooth_mid = 0.0f, smooth_treble = 0.0f;
    float smooth_energy = 0.0f;

    // Precompute the FFT bin range for each log-spaced band.
    std::array<int, kBandCount + 1> edges{};
    for (int i = 0; i <= kBandCount; ++i)
    {
        const float t = (float)i / (float)kBandCount;
        const float hz = 38.0f * std::pow(16000.0f / 38.0f, t);
        edges[i] = std::max(1, std::min(kFftSize / 2 - 1,
                                        (int)std::lround(hz * kFftSize / sample_rate)));
        if (i && edges[i] <= edges[i - 1])
            edges[i] = edges[i - 1] + 1;
    }

    while (true)
    {
        const size_t got = fread(pcm.data(), sizeof(float), pcm.size(), pipe);
        if (got < 64)
        {
            if (feof(pipe))
                break;
            continue;
        }
        const size_t nframes = got / 2;
        if (nframes < kFftSize)
            continue;

        // Mono the newest window and stash a downsampled oscilloscope trace.
        for (int i = 0; i < kFftSize; ++i)
        {
            const float s = 0.5f * (pcm[(nframes - kFftSize + i) * 2] +
                                    pcm[(nframes - kFftSize + i) * 2 + 1]);
            const float win = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f *
                                                     (float)i / (float)(kFftSize - 1));
            spectrum[i] = std::complex<float>(s * win, 0.0f);
        }
        const size_t step = std::max<size_t>(1, nframes / kWaveCount);
        for (int i = 0; i < kWaveCount; ++i)
        {
            const size_t idx = std::min(nframes - 1, (size_t)i * step);
            const float s = 0.5f * (pcm[idx * 2] + pcm[idx * 2 + 1]);
            g_wave[i].store(std::max(-1.0f, std::min(1.0f, s * 2.4f)),
                            std::memory_order_relaxed);
        }

        fft(spectrum);
        float bass = 0.0f, mid = 0.0f, treble = 0.0f;
        for (int b = 0; b < kBandCount; ++b)
        {
            float sum = 0.0f;
            int count = 0;
            for (int k = edges[b]; k < edges[b + 1] && k < kFftSize / 2; ++k)
            {
                sum += std::abs(spectrum[k]);
                ++count;
            }
            const float mag = count ? sum / (float)count : 0.0f;
            const float db = 20.0f * std::log10(mag + 1e-7f);
            // -66 dB is silence, -8 dB is a very loud band. Keep a little
            // headroom and apply a gentle curve so quiet detail stays visible.
            float v = (db + 66.0f) / 58.0f;
            v = std::max(0.0f, std::min(1.0f, v));
            v = std::pow(v, 1.35f);
            // Fast attack, slow release reads as "alive" without flicker.
            const float prev = smooth[b];
            const float speed = v > prev ? 0.62f : 0.16f;
            smooth[b] = prev + (v - prev) * speed;
            peak[b] = std::max(peak[b] - 0.012f, smooth[b]);
            g_bands[b].store(smooth[b], std::memory_order_relaxed);
            if (b < 9)
                bass += smooth[b];
            else if (b < 30)
                mid += smooth[b];
            else
                treble += smooth[b];
        }
        bass /= 9.0f;
        mid /= 21.0f;
        treble /= (float)(kBandCount - 30);
        const float energy = std::max(bass, std::max(mid * 0.86f, treble * 0.72f));
        smooth_bass += (bass - smooth_bass) * 0.35f;
        smooth_mid += (mid - smooth_mid) * 0.30f;
        smooth_treble += (treble - smooth_treble) * 0.26f;
        smooth_energy += (energy - smooth_energy) * 0.30f;
        g_bass.store(smooth_bass, std::memory_order_relaxed);
        g_mid.store(smooth_mid, std::memory_order_relaxed);
        g_treble.store(smooth_treble, std::memory_order_relaxed);
        g_energy.store(smooth_energy, std::memory_order_relaxed);
    }
    pclose(pipe);
    g_capture_running.store(false);
    g_capture_state.store(2);
    publish_silence();
}

void ensure_capture()
{
    if (g_capture_started.exchange(true))
        return;
    g_capture_state.store(0);
    std::thread(capture_worker).detach();
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
ImU32 lerp_col(ImU32 a, ImU32 b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const int ar = (a >> 0) & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    const int br = (b >> 0) & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    return IM_COL32((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t),
                    (int)(ab + (bb - ab) * t), (int)(((a >> 24) & 0xFF) +
                                                     (((b >> 24) & 0xFF) - ((a >> 24) & 0xFF)) * t));
}

ImU32 with_alpha(ImU32 col, float alpha)
{
    const int a = (int)(((col >> 24) & 0xFF) * std::max(0.0f, std::min(1.0f, alpha)));
    return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

ImU32 freq_col(float t, float alpha)
{
    const ImU32 accent = ui_accent_u32(1.0f);
    const ImU32 cool = IM_COL32(74, 205, 255, 255);
    const ImU32 violet = IM_COL32(174, 122, 255, 255);
    const ImU32 mixed = t < 0.58f ? lerp_col(accent, cool, t / 0.58f)
                                  : lerp_col(cool, violet, (t - 0.58f) / 0.42f);
    return with_alpha(mixed, alpha);
}

float band(int i)
{
    i = std::max(0, std::min(kBandCount - 1, i));
    return g_bands[i].load(std::memory_order_relaxed);
}

float wave_at(int i)
{
    i = std::max(0, std::min(kWaveCount - 1, i));
    return g_wave[i].load(std::memory_order_relaxed);
}

float eased(float &cur, float target, float speed)
{
    cur = ui_anim_approach(cur, target, speed);
    return cur;
}

// Layered circles approximate a soft radial gradient without a shader pass.
// A single translucent circle reads as a hard disc on OLED; stacking a dozen
// with geometrically shrinking radii blends into a believable bloom.
void soft_glow(ImDrawList *dl, ImVec2 centre, float radius, ImU32 colour, float alpha)
{
    constexpr int rings = 14;
    for (int i = 0; i < rings; ++i)
    {
        const float t = (float)i / (float)(rings - 1);
        const float r = radius * (0.16f + 0.84f * t);
        dl->AddCircleFilled(centre, r, with_alpha(colour, alpha * 0.075f), 48);
    }
}

void draw_glass_card(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float rounding)
{
    dl->AddRectFilled(p0, p1, IM_COL32(12, 16, 23, 196), rounding);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 26), rounding, 0, 1.0f);
}

void draw_backdrop(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float energy, double t)
{
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    dl->AddRectFilledMultiColor(p0, p1,
                                IM_COL32(9, 12, 18, 255), IM_COL32(12, 14, 24, 255),
                                IM_COL32(7, 9, 15, 255), IM_COL32(5, 7, 11, 255));
    // Slow, huge colour clouds. Their alpha is driven by the low end so the
    // room itself seems to breathe with the kick.
    const float base = 0.060f + energy * 0.13f;
    for (int i = 0; i < 4; ++i)
    {
        const float k = (float)i / 3.0f;
        const float cx = p0.x + w * (0.5f + 0.30f * std::sin((float)t * 0.09f + i * 1.9f));
        const float cy = p0.y + h * (0.45f + 0.25f * std::cos((float)t * 0.07f + i * 1.3f));
        const float radius = std::min(w, h) * (0.34f + 0.12f * k);
        soft_glow(dl, ImVec2(cx, cy), radius,
                  freq_col(0.15f + k * 0.65f, 1.0f), base * (1.0f - 0.16f * k));
    }
    // Very faint scanning grid, only visible up close.
    const float step = 46.0f * ui_m().layout_scale;
    for (float x = p0.x + step; x < p1.x; x += step)
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 5));
    for (float y = p0.y + step; y < p1.y; y += step)
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 255, 255, 5));
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 18), ui_m().radius_lg, 0, 1.0f);
}

void draw_art_wash(ImDrawList *dl, ImVec2 p0, ImVec2 p1, const std::string &url, float alpha)
{
    if (url.empty())
        return;
    const float side = std::min(p1.x - p0.x, p1.y - p0.y);
    const ImVec2 c((p0.x + p1.x) * 0.5f, p0.y + (p1.y - p0.y) * 0.48f);
    const ImVec2 a0(c.x - side * 0.46f, c.y - side * 0.46f);
    const ImVec2 a1(c.x + side * 0.46f, c.y + side * 0.46f);
    ui_draw_cover(dl, url, a0, a1, side * 0.46f, alpha, UiIcon::Visualizer,
                  "visualizer_wash", true, true);
    // Fade the edges into the backdrop with a dark vignette.
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p1.y),
                                IM_COL32(5, 7, 11, 150), IM_COL32(5, 7, 11, 110),
                                IM_COL32(5, 7, 11, 235), IM_COL32(5, 7, 11, 235));
}

// A floating particle field. Positions are deterministic and live on a fixed
// unit grid so resizing never makes them jump.
struct Particle
{
    float x, y, speed, size, phase;
};
std::array<Particle, 84> g_particles = [] {
    std::array<Particle, 84> p{};
    unsigned int seed = 0x9e3779b9u;
    auto rnd = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (seed & 0xFFFFFFu) / (float)0xFFFFFF;
    };
    for (auto &q : p)
    {
        q.x = rnd();
        q.y = rnd();
        q.speed = 0.006f + rnd() * 0.022f;
        q.size = 0.8f + rnd() * 2.2f;
        q.phase = rnd() * 6.28318f;
    }
    return p;
}();

void draw_particles(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float energy, float treble, double t)
{
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const int count = 40 + (int)(treble * 34.0f);
    for (int i = 0; i < count; ++i)
    {
        const Particle &q = g_particles[i];
        float y = q.y - std::fmod((float)t * (q.speed + energy * 0.05f) + q.phase * 0.05f, 1.0f);
        if (y < 0.0f)
            y += 1.0f;
        const float x = q.x + 0.018f * std::sin((float)t * 0.5f + q.phase + y * 3.0f);
        const ImVec2 pos(p0.x + x * w, p0.y + y * h);
        const float size = q.size * ui_m().layout_scale * (0.8f + treble * 0.9f);
        const float a = 0.18f + 0.34f * (0.5f + 0.5f * std::sin((float)t * 1.4f + q.phase));
        dl->AddCircleFilled(pos, size * 2.6f, freq_col(q.x, a * 0.22f), 12);
        dl->AddCircleFilled(pos, size, freq_col(q.x, a), 10);
    }
}

// -- Bars -------------------------------------------------------------------
void draw_bars(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float energy, float treble, double t)
{
    const UiMetrics &m = ui_m();
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const float cx = p0.x + w * 0.5f;
    const float base = p0.y + h * 0.61f;
    const float max_h = h * 0.36f;
    const float usable = w * 0.70f;
    const int half = 34;
    const float slot = usable / (float)half;
    const float bw = std::max(2.0f, slot * 0.52f);

    static std::array<float, half> peak{};
    static std::array<float, half> smooth{};

    // Central glow that swells with bass.
    const float pulse = 0.5f + 0.5f * std::sin((float)t * 2.4f);
    soft_glow(dl, ImVec2(cx, base - max_h * 0.16f),
              std::min(w, h) * (0.10f + energy * 0.13f),
              freq_col(0.42f, 1.0f), 0.38f + energy * 0.55f);

    for (int side = 0; side < 2; ++side)
    {
        const float dir = side == 0 ? -1.0f : 1.0f;
        for (int i = 0; i < half; ++i)
        {
            const int bi = (int)(std::pow((float)i / (float)(half - 1), 1.55f) * (kBandCount - 1));
            const float target = std::max(0.012f, band(bi));
            smooth[i] = eased(smooth[i], target, target > smooth[i] ? 26.0f : 8.0f);
            peak[i] = std::max(peak[i] - 0.020f, smooth[i]);
            const float v = smooth[i];
            const float bh = v * max_h;
            const float x = cx + dir * ((float)i + 0.5f) * slot;
            const ImVec2 a(x - bw * 0.5f, base - bh);
            const ImVec2 b(x + bw * 0.5f, base);
            const float k = (float)i / (float)(half - 1);
            const ImU32 col = freq_col(k, 0.62f + 0.30f * v);

            // Soft bloom behind each bar.
            dl->AddRectFilled(ImVec2(a.x - bw * 0.8f, a.y - 4.0f * m.layout_scale),
                              ImVec2(b.x + bw * 0.8f, b.y + 4.0f * m.layout_scale),
                              with_alpha(col, 0.10f + 0.09f * v), bw, 24);
            dl->AddRectFilled(a, b, col, bw * 0.5f, 20);
            // Hot core.
            dl->AddRectFilled(ImVec2(a.x + bw * 0.28f, a.y + 1.0f),
                              ImVec2(a.x + bw * 0.58f, b.y), with_alpha(col, 0.42f), bw * 0.18f, 8);

            // Peak cap.
            const float py = base - peak[i] * max_h;
            dl->AddRectFilled(ImVec2(x - bw * 0.6f, py - 1.4f * m.layout_scale),
                              ImVec2(x + bw * 0.6f, py + 1.4f * m.layout_scale),
                              with_alpha(col, 0.55f), 2.0f);

            // A soft mirrored floor reflection.
            for (int r = 0; r < 3; ++r)
            {
                const float rh = bh * (0.30f - 0.075f * r);
                if (rh < 1.0f)
                    continue;
                dl->AddRectFilled(ImVec2(a.x, base + 4.0f + rh * (float)r),
                                  ImVec2(b.x, base + 4.0f + rh * (float)(r + 1)),
                                  freq_col(k, 0.055f - 0.012f * r), bw * 0.5f);
            }
        }
    }

    // Baseline light streak.
    dl->AddRectFilledMultiColor(ImVec2(cx - usable * 0.53f, base - 1.0f),
                                ImVec2(cx + usable * 0.53f, base + 1.5f),
                                ui_accent_u32(0.0f), ui_accent_u32(0.55f),
                                ui_accent_u32(0.0f), ui_accent_u32(0.0f));
    dl->AddRectFilled(ImVec2(cx - usable * 0.53f, base + 1.5f),
                      ImVec2(cx + usable * 0.53f, base + 2.0f), IM_COL32(255, 255, 255, 18));
    draw_particles(dl, p0, p1, energy, treble, t);
}

// -- Waves ------------------------------------------------------------------
void draw_waves(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float energy, float treble, double t)
{
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const float cy = p0.y + h * 0.50f;
    const int steps = 220;

    // The raw PCM can be quiet on spoken-word content. Normalise the visible
    // trace against its own peak so the mode stays expressive at any volume.
    float peak = 0.02f;
    for (int i = 0; i < kWaveCount; ++i)
        peak = std::max(peak, std::fabs(wave_at(i)));
    const float gain = std::min(2.4f, 0.78f / peak);
    const float amp = h * (0.095f + energy * 0.22f);

    // A translucent frequency "curtain" behind the trace: smooth vertical
    // bars top and bottom, deliberately quieter than the Bars mode.
    for (int b = 0; b < kBandCount; ++b)
    {
        const float u = (float)b / (float)(kBandCount - 1);
        const float v = std::pow(band(b), 0.72f);
        const float bh = h * (0.025f + v * 0.13f);
        const float x = p0.x + u * w;
        const float a = 0.045f + v * 0.075f;
        dl->AddLine(ImVec2(x, p0.y + h * 0.20f), ImVec2(x, p0.y + h * 0.20f + bh),
                    freq_col(u, a), 2.0f);
        dl->AddLine(ImVec2(x, p0.y + h * 0.80f), ImVec2(x, p0.y + h * 0.80f - bh),
                    freq_col(u, a), 2.0f);
    }

    // Three oscilloscope traces driven by the actual PCM samples. Each one
    // is a glowing ribbon: broad translucent pass, then a crisp highlight.
    for (int trace = 0; trace < 3; ++trace)
    {
        const float phase = (float)t * (0.5f + trace * 0.18f) + trace * 2.0f;
        const float scale = 1.0f - trace * 0.16f;
        const float a = 0.58f - trace * 0.12f;
        for (int pass = 0; pass < 3; ++pass)
        {
            dl->PathClear();
            for (int i = 0; i <= steps; ++i)
            {
                const float u = (float)i / (float)steps;
                const int wi = (int)(u * (kWaveCount - 1));
                const float sample = wave_at(wi) * gain;
                const float envelope = std::sin(u * 3.14159f);
                const float y = cy + sample * amp * scale * envelope +
                                std::sin(u * 12.0f + phase) * amp * 0.05f;
                dl->PathLineTo(ImVec2(p0.x + u * w, y));
            }
            const float thick = pass == 0 ? 18.0f : (pass == 1 ? 5.0f : 1.7f);
            const float alpha = pass == 0 ? 0.12f : (pass == 1 ? 0.38f : 0.98f);
            dl->PathStroke(freq_col(0.20f + trace * 0.24f, a * alpha), false, thick);
        }
    }

    // Mirror a faint trace below for symmetry.
    for (int pass = 0; pass < 2; ++pass)
    {
        dl->PathClear();
        for (int i = 0; i <= steps; ++i)
        {
            const float u = (float)i / (float)steps;
            const float sample = wave_at((int)(u * (kWaveCount - 1))) * gain;
            const float envelope = std::sin(u * 3.14159f);
            const float y = cy - sample * amp * 0.45f * envelope + amp * 0.75f;
            dl->PathLineTo(ImVec2(p0.x + u * w, y));
        }
        dl->PathStroke(freq_col(0.62f, pass == 0 ? 0.12f : 0.32f), false,
                       pass == 0 ? 14.0f : 2.0f);
    }

    // A slow drifting lens flare at the centre.
    soft_glow(dl, ImVec2(p0.x + w * (0.5f + 0.10f * std::sin((float)t * 0.3f)), cy),
              std::min(w, h) * (0.035f + energy * 0.06f),
              freq_col(0.45f, 1.0f), 0.35f + energy * 0.45f);
    draw_particles(dl, p0, p1, energy, treble, t);
}

// -- Orbits -----------------------------------------------------------------
void draw_orbits(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float energy, float treble,
                 double t, const std::string &art_url)
{
    const UiMetrics &m = ui_m();
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const float cx = p0.x + w * 0.5f;
    const float cy = p0.y + h * 0.50f;
    const float base = std::min(w, h) * 0.22f;
    const float pulse = 1.0f + energy * 0.10f;

    // Central album art disc.
    const float art_r = base * (0.66f + 0.02f * std::sin((float)t * 2.0f));
    soft_glow(dl, ImVec2(cx, cy), art_r + 10.0f * m.layout_scale,
              freq_col(0.25f, 1.0f), 0.45f + energy * 0.45f);
    const ImVec2 a0(cx - art_r, cy - art_r);
    const ImVec2 a1(cx + art_r, cy + art_r);
    if (!art_url.empty())
        ui_draw_cover(dl, art_url, a0, a1, art_r, 1.0f, UiIcon::Visualizer,
                      "visualizer_art", true, true);
    else
    {
        dl->AddCircleFilled(ImVec2(cx, cy), art_r, freq_col(0.35f, 0.20f), 72);
        ui_draw_icon(dl, UiIcon::Visualizer, ImVec2(cx, cy), art_r * 0.60f,
                     IM_COL32(235, 242, 255, 235), 1.8f);
    }
    dl->AddCircle(ImVec2(cx, cy), art_r + 1.0f, IM_COL32(255, 255, 255, 70), 96, 1.2f);

    // Radial spectrum: 96 spokes, log-mapped into the 64 bands.
    const int spokes = 96;
    for (int i = 0; i < spokes; ++i)
    {
        const float u = (float)i / (float)spokes;
        // Mirror low frequencies at both sides, high at the top.
        const float tri = 1.0f - std::fabs(u * 2.0f - 1.0f);
        const int bi = (int)(std::pow(tri, 1.35f) * (kBandCount - 1));
        const float v = std::pow(band(bi), 0.72f);
        const float angle = u * 6.28318f - 3.14159265f * 0.5f + (float)t * 0.07f;
        const float r0 = base * (1.06f + 0.02f * std::sin((float)t * 0.9f + i * 0.21f));
        const float r1 = r0 + base * (0.14f + v * 0.82f) * pulse;
        const ImVec2 q0(cx + std::cos(angle) * r0, cy + std::sin(angle) * r0);
        const ImVec2 q1(cx + std::cos(angle) * r1, cy + std::sin(angle) * r1);
        const float k = (float)i / (float)spokes;
        const ImU32 col = freq_col(k, 0.30f + v * 0.66f);
        dl->AddLine(ImVec2(cx + std::cos(angle) * (r0 - 2.0f), cy + std::sin(angle) * (r0 - 2.0f)),
                    ImVec2(cx + std::cos(angle) * (r1 + 3.0f), cy + std::sin(angle) * (r1 + 3.0f)),
                    with_alpha(col, 0.075f + v * 0.10f), 6.0f * m.layout_scale);
        dl->AddLine(q0, q1, col, std::max(1.1f, 2.2f * m.layout_scale));
        dl->AddCircleFilled(q1, std::max(1.2f, 2.0f * m.layout_scale) * (0.7f + v),
                            with_alpha(col, 0.65f), 10);
    }

    // Three orbit rings with glowing satellites.
    for (int ring = 0; ring < 3; ++ring)
    {
        const float rr = base * (1.20f + 0.16f * ring) * (1.0f + energy * 0.03f * (ring + 1));
        const float speed = 0.18f + 0.06f * ring;
        dl->AddCircle(ImVec2(cx, cy), rr, freq_col(0.25f + ring * 0.2f, 0.10f + 0.02f * ring),
                      128, 1.0f);
        const int dots = 3 + ring;
        for (int d = 0; d < dots; ++d)
        {
            const float a = (float)t * speed + d * (6.28318f / dots) + ring * 1.3f;
            const ImVec2 pos(cx + std::cos(a) * rr, cy + std::sin(a) * rr);
            const float dr = (2.0f + treble * 3.0f) * m.layout_scale;
            const ImU32 col = freq_col(0.30f + ring * 0.22f, 0.75f);
            dl->AddCircleFilled(pos, dr * 3.4f, with_alpha(col, 0.08f), 16);
            dl->AddCircleFilled(pos, dr, col, 14);
        }
    }

    // Bass rings expanding outwards.
    const float beat = g_bass.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
    {
        const float phase = std::fmod((float)t * 0.55f + i * 0.33f, 1.0f);
        const float rr = base * (1.04f + phase * 2.1f);
        dl->AddCircle(ImVec2(cx, cy), rr, freq_col(0.55f, (1.0f - phase) * beat * 0.22f),
                      96, 1.5f);
    }
    draw_particles(dl, p0, p1, energy, treble, t);
}

void draw_mode_chip(ImDrawList *dl, AppState &state, ImVec2 pos, float bw, float bh,
                    const char *label, int index, int mode)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(label, ImVec2(bw, bh));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
    {
        state.settings.visualizer_mode = index;
        save_settings(state.settings);
        state.repaint_requested = true;
    }
    static float hover_anim[3] = {0, 0, 0};
    hover_anim[index] = ui_anim_approach(hover_anim[index], hovered ? 1.0f : 0.0f, 18.0f);
    if (std::fabs(hover_anim[index] - (hovered ? 1.0f : 0.0f)) > 0.002f)
        ui_anim_request();
    const bool active = index == mode;
    const ImVec2 p0 = pos;
    const ImVec2 p1(pos.x + bw, pos.y + bh);
    const ImU32 fill = active ? ui_accent_u32(0.26f)
                              : IM_COL32(255, 255, 255, (int)(9 + 12 * hover_anim[index]));
    dl->AddRectFilled(p0, p1, fill, bh * 0.5f);
    dl->AddRect(p0, p1, active ? ui_accent_u32(0.55f) : IM_COL32(255, 255, 255, 24),
                bh * 0.5f, 0, 1.0f);
    const ImU32 text = active ? IM_COL32(248, 251, 255, 255) : IM_COL32(184, 194, 210, 235);
    const ImVec2 ts = g_ui_fonts.medium->CalcTextSizeA(16.0f * ui_m().text_scale, FLT_MAX, 0, label);
    dl->AddText(g_ui_fonts.medium, 16.0f * ui_m().text_scale,
                ImVec2(p0.x + (bw - ts.x) * 0.5f, p0.y + (bh - ts.y) * 0.5f), text, label);
}
} // namespace

void draw_visualizer_panel(AppState &state)
{
    ensure_capture();
    const UiMetrics &m = ui_m();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = std::max(360.0f, avail.x);
    const float h = std::max(420.0f, avail.y - 8.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const double now = ImGui::GetTime();

    const float energy = g_energy.load(std::memory_order_relaxed);
    const float bass = g_bass.load(std::memory_order_relaxed);
    const float treble = g_treble.load(std::memory_order_relaxed);
    const bool live = g_capture_running.load(std::memory_order_relaxed) && state.playing;

    draw_backdrop(dl, p0, p1, energy, now);

    const int mode = std::max(0, std::min(2, state.settings.visualizer_mode));
    if (mode == 0)
        draw_bars(dl, p0, p1, energy, treble, now);
    else if (mode == 1)
        draw_waves(dl, p0, p1, energy, treble, now);
    else
        draw_orbits(dl, p0, p1, energy, treble, now, state.current_thumbnail_url);

    ui_anim_request();

    // Top row: mode chips + a live-audio indicator.
    const float pad = m.space_5;
    const float chip_h = std::max(30.0f * m.layout_scale, m.control_h * 0.78f);
    const char *labels[] = {"Bars", "Waves", "Orbits"};
    float x = p0.x + pad;
    for (int i = 0; i < 3; ++i)
    {
        const float bw = 86.0f * m.layout_scale;
        draw_mode_chip(dl, state, ImVec2(x, p0.y + pad), bw, chip_h, labels[i], i, mode);
        x += bw + m.space_2;
    }

    // Live indicator top-right with a breathing dot.
    const char *state_label = live ? "LIVE AUDIO" : (state.playing ? "ANALYSING" : "IDLE");
    const ImU32 dot_col = live ? IM_COL32(97, 255, 177, 255) : IM_COL32(170, 180, 196, 170);
    const ImVec2 ts = g_ui_fonts.medium->CalcTextSizeA(12.0f * m.text_scale, FLT_MAX, 0,
                                                       state_label);
    const float iw = ts.x + 30.0f * m.layout_scale;
    const float ix = p1.x - pad - iw;
    const float iy = p0.y + pad + (chip_h - 24.0f * m.layout_scale) * 0.5f;
    draw_glass_card(dl, ImVec2(ix - 12.0f * m.layout_scale, iy),
                    ImVec2(ix + iw, iy + 24.0f * m.layout_scale), 12.0f * m.layout_scale);
    const float breath = live ? (0.55f + 0.45f * std::sin((float)now * 3.0f)) : 0.35f;
    dl->AddCircleFilled(ImVec2(ix + 4.0f * m.layout_scale, iy + 12.0f * m.layout_scale),
                        10.0f * m.layout_scale, with_alpha(dot_col, 0.10f), 16);
    dl->AddCircleFilled(ImVec2(ix + 4.0f * m.layout_scale, iy + 12.0f * m.layout_scale),
                        3.2f * m.layout_scale, with_alpha(dot_col, breath), 12);
    dl->AddText(g_ui_fonts.medium, 12.0f * m.text_scale,
                ImVec2(ix + 16.0f * m.layout_scale, iy + 5.0f * m.layout_scale),
                IM_COL32(205, 214, 228, 235), state_label);

    // Bottom now-playing glass bar.
    const float art = 58.0f * m.layout_scale;
    const float bar_h = art + m.space_3 * 2.0f;
    const ImVec2 b0(p0.x + pad, p1.y - pad - bar_h);
    const ImVec2 b1(p1.x - pad, p1.y - pad);
    draw_glass_card(dl, b0, b1, m.radius_lg);
    const ImVec2 a0(b0.x + m.space_3, b0.y + m.space_3);
    ui_draw_cover(dl, state.current_thumbnail_url, a0,
                  ImVec2(a0.x + art, a0.y + art), m.radius_md, 0.98f,
                  UiIcon::Visualizer, "visualizer_now", false, true);

    const float tx = a0.x + art + m.space_3;
    const float max_tw = b1.x - tx - m.space_4 - 48.0f * m.layout_scale;
    const std::string title = state.current_title.empty() ? "Nothing playing" : state.current_title;
    const std::string artist = state.current_artist.empty() ? "Pick something to light up"
                                                             : state.current_artist;
    dl->AddText(g_ui_fonts.semibold, 20.0f * m.text_scale,
                ImVec2(tx, a0.y + 3.0f * m.layout_scale), IM_COL32(246, 249, 255, 255),
                ui_ellipsize(g_ui_fonts.semibold, 20.0f, title, std::max(80.0f, max_tw)).c_str());
    dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                ImVec2(tx, a0.y + 27.0f * m.layout_scale), IM_COL32(172, 184, 203, 235),
                ui_ellipsize(g_ui_fonts.body, 15.0f, artist, std::max(80.0f, max_tw)).c_str());

    // Tiny spectrum readout under the title so the card feels connected to
    // the big visual above it.
    const float meter_y = b1.y - m.space_3 - 4.0f * m.layout_scale;
    for (int i = 0; i < 22; ++i)
    {
        const float v = band((int)((float)i / 22.0f * (kBandCount - 1)));
        const float mh = 3.0f * m.layout_scale + v * 9.0f * m.layout_scale;
        const float mx = tx + (float)i * 7.0f * m.layout_scale;
        dl->AddRectFilled(ImVec2(mx, meter_y - mh), ImVec2(mx + 3.5f * m.layout_scale, meter_y),
                          freq_col((float)i / 22.0f, 0.30f + v * 0.55f), 2.0f);
    }

    ImGui::SetCursorScreenPos(ImVec2(b1.x - m.space_3 - 44.0f * m.layout_scale,
                                     b0.y + (bar_h - 44.0f * m.layout_scale) * 0.5f));
    if (ui_icon_button("##visualizer_play", state.playing ? UiIcon::Pause : UiIcon::Play,
                       44.0f * m.layout_scale, false, IM_COL32(235, 242, 255, 255),
                       state.playing ? "Pause" : "Play"))
    {
        if (state.playing)
            state.user_pause_requested = true;
        else
            state.user_play_requested = true;
        state.repaint_requested = true;
    }

    // Restore the cursor below the canvas for the layout pass.
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + m.space_2));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}
