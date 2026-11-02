#include "mpvplayer.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

MpvPlayer::MpvPlayer(const std::string &browser)
{
    mpv_handle *h = mpv_create();
    mpv.reset(h);

    mpv_set_option_string(h, "video", "no");
    mpv_set_option_string(h, "terminal", "no");
    mpv_set_option_string(h, "msg-level", "all=status");

    mpv_set_option_string(h, "ytdl", "yes");
    mpv_set_option_string(h, "ytdl-format", "bestaudio");
    // Direct googlevideo streams are 403 without a YouTube referrer.
    mpv_set_option_string(h, "referrer", "https://www.youtube.com/");
    mpv_set_option_string(h, "user-agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    std::string ytdl_opts = "cookies-from-browser=" + browser +
                            ",user-agent=Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    mpv_set_option_string(h, "ytdl-raw-options", ytdl_opts.c_str());

    // Crossfade
    mpv_set_option_string(h, "audio-crossfade", "5.0");
    mpv_set_option_string(h, "gapless-audio", "yes");

    mpv_initialize(h);

    // Observe the properties hot paths read every frame; the change events
    // are drained by process_events() and folded into the cached atomics.
    mpv_observe_property(h, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(h, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(h, 0, "volume", MPV_FORMAT_DOUBLE);
}

MpvPlayer::~MpvPlayer() = default;

void MpvPlayer::play(const std::string &url)
{
    seeked = false; // new file: any prior seek no longer applies
    // Replacing the current file makes mpv report END_FILE reason STOP for it;
    // remember that so process_events() doesn't pass it off as a real stop.
    replacing = true;
    // Direct stream URLs (and local files) must not be handed to mpv's
    // youtube-dl hook, otherwise it tries to "extract" a googlevideo URL and
    // fails with 403. Only genuine YouTube watch URLs need ytdl.
    bool needs_ytdl = (url.find("youtube.com/watch") != std::string::npos) ||
                      (url.find("youtu.be/") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(prop_mutex);
        mpv_set_property_string(mpv.get(), "ytdl", needs_ytdl ? "yes" : "no");
    }
    const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
    mpv_command_async(mpv.get(), 0, cmd);
}

void MpvPlayer::set_crossfade(double seconds)
{
    set_property("audio-crossfade", std::to_string(seconds));
}

void MpvPlayer::set_speed(double speed)
{
    set_property("speed", std::to_string(speed));
}

void MpvPlayer::set_af(const std::string &af)
{
    set_property("af", af); // empty string clears the filter chain
}

void MpvPlayer::load_next(const std::string &url)
{
    const char *cmd[] = {"loadfile", url.c_str(), "append-play", nullptr};
    mpv_command_async(mpv.get(), 0, cmd);
}

void MpvPlayer::stop()
{
    replacing = false; // a genuine stop: any pending swap marker is void
    mpv_command_async(mpv.get(), 0, (const char *[]){"stop", nullptr});
    playing = false;
}

void MpvPlayer::toggle_pause()
{
    mpv_command_async(mpv.get(), 0, (const char *[]){"cycle", "pause", nullptr});
}

void MpvPlayer::set_paused(bool paused)
{
    // Explicit state set: unlike toggle_pause(), this cannot get out of
    // sync with the UI's belief about playback state (Play/Pause from
    // the button, MPRIS, media keys, session restore, ...).
    std::lock_guard<std::mutex> lock(prop_mutex);
    int flag = paused ? 1 : 0;
    mpv_set_property(mpv.get(), "pause", MPV_FORMAT_FLAG, &flag);
}

bool MpvPlayer::is_playing() const
{
    return playing;
}

void MpvPlayer::set_event_callback(EventCallback cb)
{
    std::lock_guard<std::recursive_mutex> lock(cb_mutex);
    event_cb = std::move(cb);
}

void MpvPlayer::process_events()
{
    while (true)
    {
        mpv_event *event = mpv_wait_event(mpv.get(), 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        std::string type;
        switch (event->event_id)
        {
        case MPV_EVENT_START_FILE:
            playing = true;
            replacing = false; // any swap marker is spent by now
            type = "start";
            break;
        case MPV_EVENT_END_FILE:
        {
            // During an audio-crossfade the next file starts before the
            // old one ends, so its END_FILE must not clear the playing
            // flag: ask mpv whether anything is still loaded/playing.
            int idle = 1;
            mpv_get_property(mpv.get(), "idle-active", MPV_FORMAT_FLAG, &idle);
            playing = !idle;
            auto *end = (mpv_event_end_file *)event->data;
            // play() replaces the current file and mpv reports that old file
            // as END_FILE reason STOP. Emitting "stop" for it would mark
            // playback as stopped in the UI, clear the listen timer and -- since
            // the "start" handler re-applies the play/pause intent from
            // g_state.playing -- leave the freshly loaded track paused until
            // the user presses play again. Consume the marker and stay silent.
            if (end->reason == MPV_END_FILE_REASON_STOP && replacing.exchange(false))
                break;
            if (end->reason == MPV_END_FILE_REASON_EOF)
                type = "end";
            else if (end->reason == MPV_END_FILE_REASON_ERROR)
            {
                type = "error";
                if (end->error < 0)
                    type += std::string(":") + mpv_error_string(end->error);
            }
            else
                type = "stop";
        }
        break;
        case MPV_EVENT_PROPERTY_CHANGE:
        {
            auto *prop = (mpv_event_property *)event->data;
            if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            {
                const double v = *(const double *)prop->data;
                if (strcmp(prop->name, "time-pos") == 0)
                    c_time_pos.store(v, std::memory_order_relaxed);
                else if (strcmp(prop->name, "duration") == 0)
                    c_duration.store(v, std::memory_order_relaxed);
                else if (strcmp(prop->name, "volume") == 0)
                    c_volume.store(v, std::memory_order_relaxed);
            }
            else if (prop->format == MPV_FORMAT_NONE)
            {
                // Property became unavailable (e.g. no file loaded).
                if (strcmp(prop->name, "time-pos") == 0)
                    c_time_pos.store(-1.0, std::memory_order_relaxed);
                else if (strcmp(prop->name, "duration") == 0)
                    c_duration.store(-1.0, std::memory_order_relaxed);
            }
            break;
        }
        default:
            break;
        }

        if (!type.empty())
        {
            std::lock_guard<std::recursive_mutex> lock(cb_mutex);
            if (event_cb)
                event_cb(type);
        }
    }
}

double MpvPlayer::get_property_double(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(prop_mutex);
    double val = 0;
    mpv_get_property(mpv.get(), name.c_str(), MPV_FORMAT_DOUBLE, &val);
    return val;
}

void MpvPlayer::set_property(const std::string &name, const std::string &value)
{
    std::lock_guard<std::mutex> lock(prop_mutex);
    mpv_set_property_string(mpv.get(), name.c_str(), value.c_str());
}

void MpvPlayer::seek(double position)
{
    seek_gen.fetch_add(1);
    // mpv only keeps --stream-record contiguous when the seek stays inside
    // the demuxer cache (it re-reads cached bytes there); a seek beyond it
    // makes the stream layer skip data and leaves a hole in the recording.
    if (!is_position_cached(position))
        seeked = true;
    std::string pos_str = std::to_string(position);
    const char *cmd[] = {"seek", pos_str.c_str(), "absolute", nullptr};
    mpv_command_async(mpv.get(), 0, cmd);
}

// True when `pos` is covered by one of the demuxer cache's seekable ranges.
bool MpvPlayer::is_position_cached(double pos) const
{
    std::lock_guard<std::mutex> lock(prop_mutex);
    mpv_node node;
    if (mpv_get_property(mpv.get(), "demuxer-cache-state", MPV_FORMAT_NODE, &node) < 0)
        return false;
    bool cached = false;
    if (node.format == MPV_FORMAT_NODE_MAP && node.u.list)
    {
        auto as_double = [](const mpv_node &v) -> double
        {
            if (v.format == MPV_FORMAT_DOUBLE)
                return v.u.double_;
            if (v.format == MPV_FORMAT_INT64)
                return (double)v.u.int64;
            return -1.0;
        };
        for (int i = 0; i < node.u.list->num && !cached; ++i)
        {
            const char *key = node.u.list->keys[i];
            const mpv_node &val = node.u.list->values[i];
            if (!key || strcmp(key, "seekable-ranges") != 0)
                continue;
            if (val.format != MPV_FORMAT_NODE_ARRAY || !val.u.list)
                continue;
            for (int j = 0; j < val.u.list->num && !cached; ++j)
            {
                const mpv_node &range = val.u.list->values[j];
                if (range.format != MPV_FORMAT_NODE_MAP || !range.u.list)
                    continue;
                double start = -1.0, end = -1.0;
                for (int k = 0; k < range.u.list->num; ++k)
                {
                    const char *rkey = range.u.list->keys[k];
                    if (!rkey)
                        continue;
                    if (strcmp(rkey, "start") == 0)
                        start = as_double(range.u.list->values[k]);
                    else if (strcmp(rkey, "end") == 0)
                        end = as_double(range.u.list->values[k]);
                }
                if (start >= 0.0 && end >= start && pos >= start && pos <= end)
                    cached = true;
            }
        }
    }
    mpv_free_node_contents(&node);
    return cached;
}

void MpvPlayer::set_stream_record(const std::string &path)
{
    set_property("stream-record", path);
}

void MpvPlayer::set_volume(double vol)
{
    // Fractional volumes matter during a fade: at the quiet start of the
    // perceptual ramp integer percent steps would be silent for a while and
    // then jump.
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", vol);
    set_property("volume", buf);
}

double probe_audio_file_duration(const std::string &path)
{
    mpv_handle *h = mpv_create();
    if (!h)
        return -1.0;
    mpv_set_option_string(h, "video", "no");
    mpv_set_option_string(h, "ao", "null"); // no sound, but the file still demuxes
    mpv_set_option_string(h, "terminal", "no");
    mpv_set_option_string(h, "msg-level", "all=no");
    if (mpv_initialize(h) < 0)
    {
        mpv_destroy(h);
        return -1.0;
    }
    // Frozen right after load: we only want the header's duration, so the
    // probe must not play (or race to EOF on short files).
    mpv_set_property_string(h, "pause", "yes");
    const char *cmd[] = {"loadfile", path.c_str(), "replace", nullptr};
    if (mpv_command_async(h, 0, cmd) < 0)
    {
        mpv_terminate_destroy(h);
        return -1.0;
    }
    double dur = -1.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (dur < 0.0 && std::chrono::steady_clock::now() < deadline)
    {
        mpv_event *ev = mpv_wait_event(h, 0.1);
        if (ev->event_id == MPV_EVENT_SHUTDOWN || ev->event_id == MPV_EVENT_END_FILE)
            break; // load error / unreadable file
        double d = 0.0;
        if (mpv_get_property(h, "duration", MPV_FORMAT_DOUBLE, &d) >= 0 && d > 0.0)
            dur = d;
    }
    mpv_terminate_destroy(h);
    return dur;
}