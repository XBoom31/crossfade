#include "texture.h"
#include "paths.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include <array>
#include <unordered_map>
#include <queue>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef P100_WINDOWS
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#endif
#include <cerrno>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include "external/stb/stb_image.h"

// -----------------------------------------------------------------------
// Cache directory
// -----------------------------------------------------------------------
static std::string cache_dir()
{
    return p100_config_path("thumb_cache");
}

static std::string cache_filename(const std::string &url)
{
    if (url.empty())
        return ""; // don't return a directory path

    std::string name;
    for (char c : url)
    {
        // Cast to unsigned char before isalnum -- passing a plain (possibly
        // signed/negative) char into <cctype> functions is undefined
        // behavior for non-ASCII bytes, which URLs can contain.
        if (isalnum((unsigned char)c))
            name += c;
        else
            name += '_';
    }
    if (name.size() > 200)
        name = name.substr(name.size() - 200);
    return cache_dir() + "/" + name;
}

void ensure_cache_dir()
{
    std::string dir = cache_dir();
    mkdir(dir.c_str(), 0755);
}

// -----------------------------------------------------------------------
// Texture cache and pending queue
// -----------------------------------------------------------------------
static std::unordered_map<std::string, TextureInfo> texture_cache;
static std::unordered_map<std::string, TextureColor> color_cache; // guarded by cache_mutex
static std::mutex cache_mutex;

struct PendingTexture
{
    std::string url;
    std::vector<unsigned char> pixels;
    int width, height;
};

static std::queue<PendingTexture> pending_queue;
static std::mutex queue_mutex;

static std::unordered_map<std::string, std::atomic<bool>> loading_flags;
static std::mutex loading_mutex;

// -----------------------------------------------------------------------
// Failed-URL backoff tracking.
//
// Without this, a thumbnail URL that fails to download or decode (dead
// link, geo-blocked, transient network error, etc.) would be retried by
// get_texture() on *every single frame* for as long as its row is on
// screen -- each retry spawning a new thread and a new `curl` subprocess.
// That's a continuous, sustained CPU/process cost that has nothing to do
// with your render loop's frame rate. Instead, we remember the failure
// and only allow a retry after kRetryBackoff has passed, so a broken URL
// costs one attempt per backoff window instead of one attempt per frame.
// -----------------------------------------------------------------------
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> failed_urls;
static std::mutex failed_mutex;
static constexpr std::chrono::seconds kRetryBackoff{60};

static void mark_failed(const std::string &url)
{
    std::lock_guard<std::mutex> lock(failed_mutex);
    failed_urls[url] = std::chrono::steady_clock::now();
}

static void clear_failed(const std::string &url)
{
    std::lock_guard<std::mutex> lock(failed_mutex);
    failed_urls.erase(url);
}

// Returns true if url failed recently and is still within its backoff
// window (i.e. should NOT be retried yet).
static bool is_in_failure_backoff(const std::string &url)
{
    std::lock_guard<std::mutex> lock(failed_mutex);
    auto it = failed_urls.find(url);
    if (it == failed_urls.end())
        return false;
    return (std::chrono::steady_clock::now() - it->second) < kRetryBackoff;
}

// -----------------------------------------------------------------------
// Helpers: disk I/O and download (with robust file size handling)
// -----------------------------------------------------------------------
static bool load_from_file(const std::string &path, std::vector<uint8_t> &data)
{
    if (path.empty())
        return false;

    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;

    // Check if it's a regular file (skip directories, etc.)
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode))
    {
        fclose(f);
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }

    long size = ftell(f);
    if (size <= 0)
    {
        fclose(f);
        return false;
    }

    // Safety: limit file size to 50 MB (adjust as needed)
    const long MAX_FILE_SIZE = 50 * 1024 * 1024;
    if (size > MAX_FILE_SIZE)
    {
        fprintf(stderr, "Warning: image file too large (%ld bytes), skipping.\n", size);
        fclose(f);
        return false;
    }

    rewind(f);
    data.resize((size_t)size);
    size_t read = fread(data.data(), 1, (size_t)size, f);
    fclose(f);
    return (read == (size_t)size);
}

static void save_to_file(const std::string &path, const std::vector<uint8_t> &data)
{
    if (path.empty() || data.empty())
        return;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static bool download_url(const std::string &url, std::vector<uint8_t> &data)
{
    if (url.empty())
        return false;
    // --fail so a 404 (e.g. a video with no maxres variant) yields no body
    // instead of an error page that would get cached as "cover"; timeouts so
    // a stalled connection can't hang a loader thread indefinitely.
    std::string cmd = "curl -sL --fail --connect-timeout 8 --max-time 30 \"" + url + "\"";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
        return false;
#ifdef P100_WINDOWS
    // _popen defaults to text mode: CRLF translation on raw image bytes
    // corrupts every download. Switch the pipe to binary before reading.
    _setmode(_fileno(pipe.get()), _O_BINARY);
#endif
    std::array<char, 4096> buffer;
    size_t total = 0;
    const size_t MAX_DOWNLOAD = 50 * 1024 * 1024; // 50 MB limit
    while (size_t n = fread(buffer.data(), 1, buffer.size(), pipe.get()))
    {
        total += n;
        if (total > MAX_DOWNLOAD)
        {
            fprintf(stderr, "Warning: download too large, truncating.\n");
            data.insert(data.end(), buffer.data(), buffer.data() + n);
            break;
        }
        data.insert(data.end(), buffer.data(), buffer.data() + n);
    }
    return !data.empty();
}

static bool download_thumbnail(const std::string &url, std::vector<uint8_t> &data);

// -----------------------------------------------------------------------
// Dominant-colour extraction.
//
// Runs once per decoded image (on the loader thread, or inline on the fast
// disk-cache path) and is cached by URL, so the backdrop can read it every
// frame for free. Vivid, mid-tone pixels get the most weight; that avoids
// muddy averages where a cover is mostly black/white and keeps the glow tied
// to the colour you actually associate with the artwork.
// -----------------------------------------------------------------------
static void store_texture_color(const std::string &url, const TextureColor &color)
{
    std::lock_guard<std::mutex> lock(cache_mutex);
    color_cache[url] = color;
}

static TextureColor compute_dominant_color(const unsigned char *pixels, int width, int height)
{
    TextureColor out;
    out.ready = true;
    if (!pixels || width <= 0 || height <= 0)
        return out;

    const int step = std::max(1, std::max(width, height) / 64);
    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_w = 0.0;
    double vivid_weight = 0.0;
    int samples = 0;

    for (int y = 0; y < height; y += step)
    {
        for (int x = 0; x < width; x += step)
        {
            const unsigned char *p = pixels + ((size_t)y * width + x) * 4;
            if (p[3] < 96)
                continue;
            ++samples;

            const float r = p[0] / 255.0f;
            const float g = p[1] / 255.0f;
            const float b = p[2] / 255.0f;
            const float mx = std::max({r, g, b});
            const float mn = std::min({r, g, b});
            const float sat = mx > 0.0001f ? (mx - mn) / mx : 0.0f;
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

            // Near-black and near-white pixels carry no usable hue.
            if (luma < 0.04f || luma > 0.97f)
                continue;

            // Prefer colourful mid-tones; keep a small base weight so a
            // completely desaturated cover still yields an average colour
            // (which the caller then rejects via `valid`).
            const float mid = std::clamp(1.0f - std::fabs(luma - 0.5f) * 0.9f, 0.15f, 1.0f);
            const float w = (0.05f + sat * sat) * mid;

            sum_r += r * w;
            sum_g += g * w;
            sum_b += b * w;
            sum_w += w;
            vivid_weight += sat * sat * mid;
        }
    }

    if (samples == 0 || sum_w < 1e-5)
        return out;

    float r = (float)(sum_r / sum_w);
    float g = (float)(sum_g / sum_w);
    float b = (float)(sum_b / sum_w);

    // Require a meaningful amount of colour in the cover. A greyscale sleeve
    // (or one dominated by neutral artwork) falls back to the app accent.
    if (vivid_weight < 0.015 * samples)
        return out;

    // Gentle saturation boost so the glow reads as colour, not grey haze.
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    r = std::clamp(luma + (r - luma) * 1.45f, 0.0f, 1.0f);
    g = std::clamp(luma + (g - luma) * 1.45f, 0.0f, 1.0f);
    b = std::clamp(luma + (b - luma) * 1.45f, 0.0f, 1.0f);

    // Keep dark covers usable as a glow and stop bright covers from blowing
    // out to white.
    const float peak = std::max({r, g, b});
    if (peak > 0.001f && peak < 0.58f)
    {
        const float k = 0.58f / peak;
        r = std::clamp(r * k, 0.0f, 1.0f);
        g = std::clamp(g * k, 0.0f, 1.0f);
        b = std::clamp(b * k, 0.0f, 1.0f);
    }
    else if (peak > 0.92f)
    {
        const float k = 0.92f / peak;
        r *= k;
        g *= k;
        b *= k;
    }

    out.r = r;
    out.g = g;
    out.b = b;
    out.valid = true;
    return out;
}

// Thumbnails arrive as full 1280x720 covers but are drawn at 48-64 px in a
// list row. Uploading the full image costs GPU memory and bandwidth for no
// visible benefit, so shrink the longest side before it reaches the texture
// cache. A simple box filter keeps the result crisp.
static void downscale_rgba(const unsigned char *src, int sw, int sh,
                           std::vector<unsigned char> &out, int &ow, int &oh)
{
    constexpr int kMaxDim = 512;
    if (sw <= kMaxDim && sh <= kMaxDim)
    {
        out.assign(src, src + (size_t)sw * (size_t)sh * 4);
        ow = sw;
        oh = sh;
        return;
    }

    const float scale = (float)kMaxDim / (float)std::max(sw, sh);
    ow = std::max(1, (int)std::lround((float)sw * scale));
    oh = std::max(1, (int)std::lround((float)sh * scale));
    out.resize((size_t)ow * (size_t)oh * 4);

    for (int y = 0; y < oh; ++y)
    {
        const int sy0 = (int)((long long)y * sh / oh);
        const int sy1 = std::max(sy0 + 1, (int)((long long)(y + 1) * sh / oh));
        for (int x = 0; x < ow; ++x)
        {
            const int sx0 = (int)((long long)x * sw / ow);
            const int sx1 = std::max(sx0 + 1, (int)((long long)(x + 1) * sw / ow));
            int r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; ++sy)
            {
                for (int sx = sx0; sx < sx1 && sx < sw; ++sx)
                {
                    const unsigned char *p = src + ((size_t)sy * sw + sx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    a += p[3];
                    ++n;
                }
            }
            if (n <= 0)
                n = 1;
            unsigned char *q = out.data() + ((size_t)y * ow + x) * 4;
            q[0] = (unsigned char)(r / n);
            q[1] = (unsigned char)(g / n);
            q[2] = (unsigned char)(b / n);
            q[3] = (unsigned char)(a / n);
        }
    }
}

// -----------------------------------------------------------------------
// Background loader thread function
// -----------------------------------------------------------------------
static void load_texture_async(const std::string &url)
{
    if (url.empty())
        return;

    std::vector<uint8_t> image_data;
    std::string fname = cache_filename(url);

    // Try disk cache first
    if (!load_from_file(fname, image_data))
    {
        // Download (with lower-resolution fallback for missing maxres)
        if (!download_thumbnail(url, image_data))
        {
            std::lock_guard<std::mutex> lock(loading_mutex);
            loading_flags.erase(url);
            mark_failed(url); // don't retry every frame -- back off instead
            return;
        }
        // Save to disk
        save_to_file(fname, image_data);
    }

    // Decode
    int width, height, channels;
    unsigned char *pixels = stbi_load_from_memory(
        image_data.data(), (int)image_data.size(),
        &width, &height, &channels, 4); // force RGBA
    if (!pixels)
    {
        std::lock_guard<std::mutex> lock(loading_mutex);
        loading_flags.erase(url);
        mark_failed(url); // corrupt/undecodeable data -- also back off
        return;
    }

    // Analyse the cover while it is already decoded (off the UI thread).
    store_texture_color(url, compute_dominant_color(pixels, width, height));

    // Shrink before upload, then push the result to the GL queue.
    PendingTexture pending;
    pending.url = url;
    downscale_rgba(pixels, width, height, pending.pixels, pending.width, pending.height);
    stbi_image_free(pixels);

    clear_failed(url); // in case an earlier attempt had failed

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        pending_queue.push(std::move(pending));
    }
}

// Decode+upload result shared by the sync disk path and the worker queue.
static TextureInfo store_texture(const std::string &url, std::vector<unsigned char> &pixels,
                                 int width, int height)
{
    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    TextureInfo info;
    info.id = tex_id;
    info.width = width;
    info.height = height;
    std::lock_guard<std::mutex> lock(cache_mutex);
    texture_cache[url] = info;
    return info;
}

// -----------------------------------------------------------------------
// Loader worker pool. A thread + a curl subprocess per thumbnail (spawned
// straight from the UI thread) made fast scrolling a fork/exec storm; a
// small shared pool with a bounded queue smooths that out.
// -----------------------------------------------------------------------
static std::vector<std::thread> g_pool;
static std::deque<std::string> g_jobs;
static std::mutex g_jobs_mutex;
static std::condition_variable g_jobs_cv;
static bool g_pool_shutdown = false;
static constexpr int kPoolSize = 3;
static constexpr size_t kMaxQueuedJobs = 64;

static void pool_worker()
{
    for (;;)
    {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(g_jobs_mutex);
            g_jobs_cv.wait(lock, [] { return g_pool_shutdown || !g_jobs.empty(); });
            if (g_pool_shutdown && g_jobs.empty())
                return;
            url = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        load_texture_async(url);
    }
}

// Returns false when the queue is full (caller must clear the loading flag
// so the URL is retried on a later frame instead of stuck "loading").
static bool enqueue_texture_load(const std::string &url)
{
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < kPoolSize; ++i)
            g_pool.emplace_back(pool_worker);
    });
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (g_jobs.size() >= kMaxQueuedJobs)
            return false;
        g_jobs.push_back(url);
    }
    g_jobs_cv.notify_one();
    return true;
}

// -----------------------------------------------------------------------
// Public functions
// -----------------------------------------------------------------------
TextureInfo texture_info(const std::string &url)
{
    TextureInfo info;
    if (url.empty())
        return info;

    // 1. In-memory cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = texture_cache.find(url);
        if (it != texture_cache.end())
            return it->second;
    }

    // 2. Check if already loading
    {
        std::lock_guard<std::mutex> lock(loading_mutex);
        auto it = loading_flags.find(url);
        if (it != loading_flags.end() && it->second.load())
            return info; // still loading
    }

    // 3. Check failure backoff -- if this URL recently failed, don't
    // hammer it again every frame; wait out the backoff window first.
    if (is_in_failure_backoff(url))
        return info;

    // 3.5. Fast path: the thumbnail is already on disk. Decode + upload
    // right here instead of round-tripping through the worker pool --
    // callers are on the UI thread and a local file decodes in a few ms,
    // so cached art is on screen on the very first frame instead of
    // flashing the placeholder while the async loader catches up.
    {
        const std::string fname = cache_filename(url);
        std::vector<uint8_t> data;
        if (load_from_file(fname, data))
        {
            int w = 0, h = 0, ch = 0;
            unsigned char *pixels = stbi_load_from_memory(
                data.data(), (int)data.size(), &w, &h, &ch, 4);
            if (pixels)
            {
                std::vector<unsigned char> rgba;
                int dw = 0, dh = 0;
                store_texture_color(url, compute_dominant_color(pixels, w, h));
                downscale_rgba(pixels, w, h, rgba, dw, dh);
                stbi_image_free(pixels);
                TextureInfo info = store_texture(url, rgba, dw, dh);
                {
                    std::lock_guard<std::mutex> lock(failed_mutex);
                    failed_urls.erase(url);
                }
                return info;
            }
            mark_failed(url); // corrupt file: let the async path re-fetch
        }
    }

    // 4. Start background load (shared worker pool, bounded queue)
    bool queued;
    {
        std::lock_guard<std::mutex> lock(loading_mutex);
        auto &flag = loading_flags[url];
        queued = enqueue_texture_load(url);
        if (queued)
            flag = true;
        else
            loading_flags.erase(url); // retry on a later frame
    }

    return info;
}

TextureColor texture_dominant_color(const std::string &url)
{
    if (url.empty())
        return TextureColor{};
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = color_cache.find(url);
    if (it == color_cache.end())
        return TextureColor{};
    return it->second;
}

// Cover-fit: the source is cropped (never deformed) to fill the box.
bool cover_uv(const TextureInfo &info, float box_w, float box_h,
              float &u0, float &v0, float &u1, float &v1)
{
    u0 = 0.0f; v0 = 0.0f; u1 = 1.0f; v1 = 1.0f;
    if (!info.id || info.width <= 0 || info.height <= 0 || box_w <= 0.0f || box_h <= 0.0f)
        return false;

    float src_aspect = (float)info.width / (float)info.height;
    float dst_aspect = box_w / box_h;

    if (src_aspect > dst_aspect)
    {
        // Source is relatively wider: crop the left/right edges.
        float crop_w = info.height * dst_aspect;
        if (crop_w >= info.width)
            return true; // already (almost) square-ish fit
        float cx = (info.width - crop_w) * 0.5f;
        u0 = cx / info.width;
        u1 = (cx + crop_w) / info.width;
    }
    else
    {
        // Source is relatively taller: crop the top/bottom edges.
        float crop_h = info.width / dst_aspect;
        if (crop_h >= info.height)
            return true;
        float cy = (info.height - crop_h) * 0.5f;
        v0 = cy / info.height;
        v1 = (cy + crop_h) / info.height;
    }
    return true;
}

// -----------------------------------------------------------------------
// Process the queue – call once per frame on the main thread
void process_texture_queue()
{
    while (true)
    {
        PendingTexture pending;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (pending_queue.empty())
                break;
            pending = std::move(pending_queue.front());
            pending_queue.pop();
        }

        // Upload to GPU
        GLuint tex_id = 0;
        glGenTextures(1, &tex_id);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pending.width, pending.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pending.pixels.data());

        // Store in cache
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            TextureInfo info;
            info.id = tex_id;
            info.width = pending.width;
            info.height = pending.height;
            texture_cache[pending.url] = info;
        }

        // Clear loading flag
        {
            std::lock_guard<std::mutex> lock(loading_mutex);
            loading_flags.erase(pending.url);
        }
    }
}

// -----------------------------------------------------------------------
// True when a decoded thumbnail is waiting for a main-thread upload.
bool has_pending_textures()
{
    std::lock_guard<std::mutex> lock(queue_mutex);
    return !pending_queue.empty();
}

// -----------------------------------------------------------------------
// Free all cached textures
void clear_texture_cache()
{
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_pool_shutdown = true;
    }
    g_jobs_cv.notify_all();
    for (auto &t : g_pool)
        if (t.joinable())
            t.join();
    g_pool.clear();

    std::lock_guard<std::mutex> lock(cache_mutex);
    for (auto &kv : texture_cache)
        glDeleteTextures(1, &kv.second.id);
    texture_cache.clear();
    color_cache.clear();
}

bool thumbnail_is_cached(const std::string &url)
{
    if (url.empty())
        return false;
    std::string fname = cache_filename(url);
    struct stat st;
    return stat(fname.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// YouTube thumbnails: not every video has a maxresdefault variant, so a 404
// on the highest resolution should fall through to the next one instead of
// leaving the track coverless forever. sddefault > hqdefault; both exist for
// effectively every video.
// True when the buffer is a WebP image (RIFF....WEBP header). stb_image
// cannot decode WebP, so these must be re-requested as JPEG.
static bool is_webp(const std::vector<uint8_t> &data)
{
    return data.size() >= 12 && !memcmp(data.data(), "RIFF", 4) &&
           !memcmp(data.data() + 8, "WEBP", 4);
}

// The JPEG equivalent of an ytimg URL: https://i.ytimg.com/vi_webp/<id>/maxresdefault.webp
// becomes https://i.ytimg.com/vi/<id>/maxresdefault.jpg. Empty when the URL
// has no webp markers.
static std::string jpeg_variant_of(const std::string &url)
{
    std::string j = url;
    size_t p = j.find("vi_webp/");
    if (p != std::string::npos)
        j.replace(p, 8, "vi/");
    p = j.find(".webp");
    if (p == std::string::npos)
        return "";
    j.replace(p, 5, ".jpg");
    return j;
}

static bool download_thumbnail(const std::string &url, std::vector<uint8_t> &data)
{
    std::vector<uint8_t> webp; // kept in case only WebP exists for this video
    if (download_url(url, data))
    {
        if (!is_webp(data))
            return true;
        // Got WebP: swap it for the JPEG variant (stored under the same
        // cache name, but decodable by stb_image) before it reaches disk.
        webp = std::move(data);
        const std::string jpeg = jpeg_variant_of(url);
        if (!jpeg.empty() && download_url(jpeg, data) && !is_webp(data))
            return true;
        data = std::move(webp);
        return true;
    }
    static const char *kFallbacks[] = {"maxresdefault", "sddefault", "hqdefault"};
    for (size_t i = 0; i + 1 < sizeof(kFallbacks) / sizeof(kFallbacks[0]); ++i)
    {
        size_t pos = url.find(kFallbacks[i]);
        if (pos == std::string::npos)
            continue;
        std::string lower = url;
        lower.replace(pos, strlen(kFallbacks[i]), kFallbacks[i + 1]);
        if (download_url(lower, data))
        {
            if (!is_webp(data))
                return true;
            if (webp.empty())
                webp = std::move(data);
            const std::string jpeg = jpeg_variant_of(lower);
            if (!jpeg.empty() && download_url(jpeg, data) && !is_webp(data))
                return true;
        }
    }
    if (!webp.empty())
    {
        data = std::move(webp);
        return true; // only WebP exists: cache it anyway, nothing to show yet
    }
    return false;
}

std::string thumbnail_cache_path(const std::string &url)
{
    if (url.empty())
        return "";
    std::string fname = cache_filename(url);
    struct stat st;
    if (stat(fname.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        return fname;
    return "";
}

std::string get_cached_thumbnail_path(const std::string &url)
{
    if (url.empty())
        return "";
    std::string fname = cache_filename(url);
    struct stat st;
    if (stat(fname.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    {
        return fname;
    }
    // Download
    std::vector<uint8_t> data;
    if (!download_thumbnail(url, data))
        return "";
    save_to_file(fname, data);
    if (stat(fname.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    {
        return fname;
    }
    return "";
}