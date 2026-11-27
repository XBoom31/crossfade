#include "ui_audio_reactive.h"

#if defined(__unix__) || defined(__APPLE__)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// ----------------------------------------------------------------------
// Monitor capture (POSIX)
//
// `parec` gives us the default sink's monitor as raw s16 mono at 8 kHz: enough
// bandwidth for loudness and broad bands, cheap enough for a UI thread to
// digest. If PulseAudio/PipeWire or parec is unavailable the thread simply
// reports inactive and every visual falls back to its idle motion.
// ----------------------------------------------------------------------

namespace
{
constexpr int kRate = 8000;
constexpr int kBlock = 160; // 20 ms

struct Shared
{
    std::atomic<float> rms{0.0f};
    std::atomic<float> bass{0.0f};
    std::atomic<float> mid{0.0f};
    std::atomic<float> treble{0.0f};
    std::atomic<float> instrument{0.0f};
    std::atomic<float> density{0.0f};
    std::atomic<float> beat{0.0f};
    std::atomic<float> pluck{0.0f};
    std::atomic<bool> active{false};
};

Shared g_shared;
std::atomic<bool> g_running{false};
std::atomic<bool> g_finished{true};
std::thread g_thread;

float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

std::string trim_line(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    return s.substr(a);
}

std::string default_monitor_device()
{
    FILE *pipe = popen("pactl get-default-sink 2>/dev/null", "r");
    if (!pipe)
        return std::string();
    char buf[256] = {0};
    std::string sink;
    if (fgets(buf, sizeof(buf), pipe))
        sink = trim_line(buf);
    pclose(pipe);
    if (sink.empty())
        return std::string();
    return sink + ".monitor";
}

// Fork parec and return the read end of its stdout, or -1.
int spawn_parec(const std::string &device, pid_t &pid)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (!device.empty())
        {
            const std::string arg = "--device=" + device;
            execlp("parec", "parec", "--format=s16le", "--rate=8000",
                   "--channels=1", "--latency-msec=40", arg.c_str(),
                   (char *)nullptr);
        }
        else
        {
            execlp("parec", "parec", "--format=s16le", "--rate=8000",
                   "--channels=1", "--latency-msec=40", "@DEFAULT_MONITOR@",
                   (char *)nullptr);
        }
        _exit(127);
    }

    close(fds[1]);
    return fds[0];
}

void publish(float rms, float bass, float mid, float treble, float instrument,
             float density, float beat, float pluck, bool active)
{
    g_shared.rms.store(rms, std::memory_order_relaxed);
    g_shared.bass.store(bass, std::memory_order_relaxed);
    g_shared.mid.store(mid, std::memory_order_relaxed);
    g_shared.treble.store(treble, std::memory_order_relaxed);
    g_shared.instrument.store(instrument, std::memory_order_relaxed);
    g_shared.density.store(density, std::memory_order_relaxed);
    g_shared.beat.store(beat, std::memory_order_relaxed);
    g_shared.pluck.store(pluck, std::memory_order_relaxed);
    g_shared.active.store(active, std::memory_order_relaxed);
}

void worker()
{
    const std::string device = default_monitor_device();
    const float dt = (float)kBlock / (float)kRate;

    // One-pole split points. Coefficients are for 8 kHz.
    const float a_low = 1.0f - std::exp(-2.0f * 3.14159265f * 160.0f / (float)kRate);
    const float a_high = 1.0f - std::exp(-2.0f * 3.14159265f * 2000.0f / (float)kRate);
    // Guitar/keys live above the kick but below the air band. A narrower
    // 320 Hz .. 2.5 kHz slice stops bass notes from dominating the loader,
    // while still catching the body and first harmonics of a strum.
    const float a_inst_low =
        1.0f - std::exp(-2.0f * 3.14159265f * 320.0f / (float)kRate);
    const float a_inst_high =
        1.0f - std::exp(-2.0f * 3.14159265f * 2500.0f / (float)kRate);

    float s_rms = 0.0f, s_bass = 0.0f, s_mid = 0.0f, s_treble = 0.0f;
    float s_instrument = 0.0f, s_density = 0.0f, s_beat = 0.0f, s_pluck = 0.0f;
    float bass_avg = 0.0f, inst_avg = 0.0f;
    double last_beat = 0.0, last_pluck = 0.0;

    while (g_running.load(std::memory_order_relaxed))
    {
        pid_t pid = -1;
        const int fd = spawn_parec(device, pid);
        if (fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        int16_t buf[kBlock * 2];
        size_t have = 0;
        bool got_any = false;

        while (g_running.load(std::memory_order_relaxed))
        {
            const ssize_t n = read(fd, (char *)buf + have,
                                   sizeof(buf) - have);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            have += (size_t)n;
            if (have < kBlock * sizeof(int16_t))
                continue;

            const int16_t *s = buf;
            float e_full = 0.0f, e_low = 0.0f, e_mid = 0.0f, e_high = 0.0f;
            float e_inst = 0.0f;
            float lp = 0.0f, lp2 = 0.0f, ilp = 0.0f, ilp2 = 0.0f;
            float prev = 0.0f;
            int crossings = 0;

            for (int i = 0; i < kBlock; ++i)
            {
                const float x = (float)s[i] / 32768.0f;
                if ((x >= 0.0f) != (prev >= 0.0f))
                    ++crossings;
                prev = x;
                lp += (x - lp) * a_low;
                lp2 += (x - lp2) * a_high;
                ilp += (x - ilp) * a_inst_low;
                ilp2 += (x - ilp2) * a_inst_high;
                const float low = lp;
                const float high = x - lp2;
                const float mid = lp2 - lp;
                const float instrument = ilp2 - ilp;
                e_full += x * x;
                e_low += low * low;
                e_mid += mid * mid;
                e_high += high * high;
                e_inst += instrument * instrument;
            }

            const float inv = 1.0f / (float)kBlock;
            const float inst_rms = std::sqrt(e_full * inv) / 0.35f;
            const float inst_bass = std::sqrt(e_low * inv) * 4.2f;
            const float inst_mid = std::sqrt(e_mid * inv) * 2.2f;
            const float inst_treble = std::sqrt(e_high * inv) * 2.6f;
            // A higher gain than the broad mid band lets a quiet guitar sit in
            // the same visual range as a full mix. The immersive view applies
            // its own slow per-track reference on top of this, so loud rows do
            // not flatten the whole song.
            const float inst_instrument = std::sqrt(e_inst * inv) * 5.2f;
            // Crossings per sample maps to visual ripple density. Around 0.30
            // is a busy full-range block; quiet or bass-only blocks sit much
            // lower and therefore draw fewer peaks.
            const float inst_density =
                ((float)crossings * inv) / 0.30f;

            const float attack = 1.0f - std::exp(-dt / 0.045f);
            const float release = 1.0f - std::exp(-dt / 0.30f);
            auto follow = [&](float &s, float v) {
                v = clamp01(v);
                s += (v - s) * (v > s ? attack : release);
            };
            follow(s_rms, inst_rms);
            follow(s_bass, inst_bass);
            follow(s_mid, inst_mid);
            follow(s_treble, inst_treble);
            // Guitar band follows faster than the broad mid band so a strum
            // has a visible attack, but still releases slowly enough to hold
            // the ribbon between plucks.
            const float inst_attack = 1.0f - std::exp(-dt / 0.030f);
            const float inst_release = 1.0f - std::exp(-dt / 0.22f);
            s_instrument += (clamp01(inst_instrument) - s_instrument) *
                            (inst_instrument > s_instrument ? inst_attack
                                                            : inst_release);
            const float d_attack = 1.0f - std::exp(-dt / 0.06f);
            const float d_release = 1.0f - std::exp(-dt / 0.25f);
            s_density += (clamp01(inst_density) - s_density) *
                         (inst_density > s_density ? d_attack : d_release);

            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // Bass envelope with a simple transient detector + decaying pulse.
            bass_avg += (inst_bass - bass_avg) * 0.035f;
            if (inst_bass > bass_avg * 1.55f + 0.035f && inst_bass > 0.06f &&
                now - last_beat > 0.20)
            {
                s_beat = 1.0f;
                last_beat = now;
            }
            s_beat *= std::exp(-dt * 6.5f);

            // Plucked/strummed instruments rise faster than a sustained vocal
            // or pad. Detect a local rise in the guitar band and keep a short
            // decaying pulse; this is what makes a quiet guitar intro feel
            // active without turning every drum hit into a "guitar" event.
            inst_avg += (inst_instrument - inst_avg) * 0.045f;
            if (inst_instrument > inst_avg * 1.30f + 0.020f &&
                inst_instrument > 0.045f && now - last_pluck > 0.11)
            {
                s_pluck = 1.0f;
                last_pluck = now;
            }
            s_pluck *= std::exp(-dt * 5.5f);

            publish(clamp01(s_rms), clamp01(s_bass), clamp01(s_mid),
                    clamp01(s_treble), clamp01(s_instrument),
                    clamp01(s_density), clamp01(s_beat), clamp01(s_pluck),
                    true);
            got_any = true;
            have = 0;
        }

        close(fd);
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }

        if (!got_any)
        {
            publish(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }

    publish(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false);
    g_finished.store(true, std::memory_order_release);
}
} // namespace

void ui_audio_start()
{
    if (g_running.exchange(true))
        return;
    g_finished.store(false, std::memory_order_release);
    g_thread = std::thread(worker);
    // Detached so an application exit while the takeover is open can never
    // trip std::terminate on a joinable thread; stop() still waits for it.
    g_thread.detach();
}

void ui_audio_stop()
{
    if (!g_running.exchange(false))
        return;
    for (int i = 0; i < 200 && !g_finished.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    publish(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false);
}

const AudioReactive &ui_audio_levels()
{
    static AudioReactive out;
    out.rms = g_shared.rms.load(std::memory_order_relaxed);
    out.bass = g_shared.bass.load(std::memory_order_relaxed);
    out.mid = g_shared.mid.load(std::memory_order_relaxed);
    out.treble = g_shared.treble.load(std::memory_order_relaxed);
    out.instrument = g_shared.instrument.load(std::memory_order_relaxed);
    out.density = g_shared.density.load(std::memory_order_relaxed);
    out.beat = g_shared.beat.load(std::memory_order_relaxed);
    out.pluck = g_shared.pluck.load(std::memory_order_relaxed);
    out.active = g_shared.active.load(std::memory_order_relaxed);
    return out;
}

#else // !POSIX

// Windows has no PulseAudio monitor and no `parec`. Keep the same API so the
// immersive lyrics view needs no platform branches; levels just stay idle,
// which is how the visuals already behave before capture starts. A WASAPI
// loopback backend can replace this stub without touching any callers.
void ui_audio_start() {}
void ui_audio_stop() {}

const AudioReactive &ui_audio_levels()
{
    static const AudioReactive idle;
    return idle;
}

#endif // POSIX
