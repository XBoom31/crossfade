#pragma once
#include <mpv/client.h>
#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <mutex>

class MpvPlayer
{
public:
    using EventCallback = std::function<void(const std::string &event_type)>;

    MpvPlayer(const std::string &browser);
    ~MpvPlayer();

    void play(const std::string &url);      // replaces playlist with this track
    void load_next(const std::string &url); // appends to playlist for crossfade
    void stop();
    void toggle_pause();
    void set_paused(bool paused);
    bool is_playing() const;
    void set_event_callback(EventCallback cb);
    void process_events();

    // Record the current stream to a file (empty string stops recording).
    // Used to save streamed audio into the offline cache without a second
    // download.
    void set_stream_record(const std::string &path);
    // Crossfade length in seconds for playlist transitions (0 disables).
    void set_crossfade(double seconds);
    // True if a seek landed outside the demuxer cache since the current
    // file was loaded: mpv then skips stream data, which punches a hole in
    // an active --stream-record, so callers discard that recording.
    bool was_seeked() const { return seeked; }
    // True when `pos` lies inside a cached, seekable region. Seeking there
    // re-reads from the cache, so the recorder keeps a contiguous file.
    bool is_position_cached(double pos) const;
    // Bumped on every seek(); lets callers detect that someone else seeked
    // (e.g. a user scrub invalidates a deferred session-restore seek).
    std::uint64_t seek_generation() const { return seek_gen.load(); }

    // ---- const added so it can be called from const contexts ----
    double get_property_double(const std::string &name) const;

    // Cached mirrors of time-pos / duration / volume, kept fresh by property
    // observers drained in process_events(). Hot per-frame readers (seek
    // bar, MPRIS position, crossfade window check) use these instead of a
    // synchronous mpv API round-trip each frame. -1 = unknown/not playing.
    double cached_time_pos() const { return c_time_pos.load(std::memory_order_relaxed); }
    double cached_duration() const { return c_duration.load(std::memory_order_relaxed); }
    double cached_volume() const { return c_volume.load(std::memory_order_relaxed); }
    void set_property(const std::string &name, const std::string &value);
    void seek(double position);
    void set_volume(double vol);
    // Playback rate (1.0 = normal). Set on both instances so it survives the
    // crossfade role swap.
    void set_speed(double speed);
    // Raw audio filter chain (mpv `af` property); "" clears it.
    void set_af(const std::string &af);

    mpv_handle *handle() const { return mpv.get(); }

private:
    struct Deleter
    {
        void operator()(mpv_handle *h)
        {
            if (h)
                mpv_destroy(h);
        }
    };
    std::unique_ptr<mpv_handle, Deleter> mpv;
    std::atomic<bool> playing{false};
    std::atomic<double> c_time_pos{-1.0};
    std::atomic<double> c_duration{-1.0};
    std::atomic<double> c_volume{-1.0};
    std::atomic<bool> seeked{false};
    std::atomic<std::uint64_t> seek_gen{0};
    // Set by play(): mpv ends the file we're replacing with reason STOP, which
    // is our track change rather than a real "stop playback" event.
    std::atomic<bool> replacing{false};
    EventCallback event_cb;
    // Recursive: the main event handler re-binds callbacks when the two
    // players swap roles at the end of a crossfade, while process_events()
    // still holds the lock it took to dispatch into that handler.
    std::recursive_mutex cb_mutex;
    mutable std::mutex prop_mutex; // mutable so const method can lock it
};

// Probe the duration of a local audio file with a throwaway libmpv instance
// (no audio/video output, never advances past the header). Returns -1 when
// the file can't be opened or has no readable duration. Used to validate a
// finished --stream-record before it's moved into the cache.
double probe_audio_file_duration(const std::string &path);