#include "app_internal.h"

#include "ytdlp.h"
#include "mpvplayer.h"
#include "crossfade.h"
#include "texture.h"
#include "ui.h"
#include "ui_notify.h"
#include "library.h"
#include "writeback.h"
#include "paths.h"
#include "discord_rpc.h"
#include "tasks.h"
#include "mpris.h"
#include "power.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

void stop_fade()
{
    if (!g_fade_player)
        return;
    if (g_cf.pending || g_cf.active)
        g_player->set_volume(g_cf.user_volume);
    g_cf.pending = false;
    g_cf.active = false;
    g_cf.manual = false;
    g_cf.track = Track{};
    g_cf.index = -1;
    g_fade_player->set_stream_record("");
    g_fade_player->stop();
    g_fade_player->set_volume(0);
}


void rebuild_queue_from_source()
{
    if (!g_state.queue_replace_from_source)
        return;
    g_state.queue_replace_from_source = false;

    const std::string want_id = g_state.queue_replace_id;
    g_state.queue_replace_id.clear();
    int start = g_state.queue_replace_index;
    g_state.queue_replace_index = 0;

    g_state.play_queue.clear();
    g_state.queue_cycle.clear();

    auto playlist = get_playlist(g_state);
    if (!playlist || playlist->empty())
    {
        g_state.queue_dirty = true;
        return;
    }

    if (!want_id.empty())
    {
        // Row indices can drift (History is a snapshot), so trust the track
        // id first and use the index only as a fallback.
        for (int i = 0; i < (int)playlist->size(); ++i)
        {
            if ((*playlist)[i].id == want_id)
            {
                start = i;
                break;
            }
        }
    }
    if (start < 0 || start >= (int)playlist->size())
        start = 0;

    g_state.queue_cycle = *playlist;
    for (Track &t : g_state.queue_cycle)
        t.from_queue_source = true;

    g_state.play_queue.reserve(playlist->size() - (size_t)start);
    for (int i = start + 1; i < (int)playlist->size(); ++i)
    {
        Track t = (*playlist)[i];
        t.from_queue_source = true;
        g_state.play_queue.push_back(std::move(t));
    }

    if (g_state.settings.shuffle_enabled && g_state.play_queue.size() > 1)
    {
        static std::mt19937 rng(
            (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::shuffle(g_state.play_queue.begin(), g_state.play_queue.end(), rng);
    }

    g_state.queue_dirty = true;
    g_state.repaint_requested = true;
}


void resolve_audio(const std::string &video_id, const std::string &video_url,
                          const std::string &title, const std::string &artist,
                          const std::string &thumb, bool fresh,
                          std::uint64_t gen)
{
    (void)title;
    (void)artist;
    (void)thumb;
    PlayRequest req;
    req.id = video_id;
    req.gen = gen;
    req.path = find_cached_audio_path(video_id);
    req.remote = false;
    if (req.path.empty())
    {
        // Not cached: play the resolved stream URL immediately. The bytes
        // are saved into the offline cache as they stream (mpv
        // stream-record), so there's no second download.
        {
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (fresh)
                g_prefetched_urls.erase(video_id);
            else
            {
                auto it = g_prefetched_urls.find(video_id);
                // An empty value is an in-flight prefetch: resolving the
                // same track ourselves below is fine, the worker's result
                // will simply serve a later request.
                if (it != g_prefetched_urls.end() && !it->second.empty())
                {
                    req.path = it->second;
                    g_prefetched_urls.erase(it);
                }
            }
        }
        if (req.path.empty())
        {
            const bool user_facing = g_state.pending_manual_play.load();
            req.path = fresh ? fetch_stream_url_fresh(video_id)
                             : fetch_stream_url(video_id);
            if (req.path.empty())
            {
                // The backend couldn't resolve a URL. Tell the user why when
                // this was their own play request (speculative prefetch
                // failures stay silent, they retry later anyway).
                BackendError err = take_backend_error();
                if (err && user_facing)
                {
                    ui_notify_backend_error(err);
                    std::lock_guard<std::mutex> lock(g_play_fail_mutex);
                    g_play_fail_hint = backend_error_user_message(err);
                }
                req.path = video_url; // mpv resolves this itself as a fallback
            }
        }
        req.remote = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_play_queue_mutex);
        g_play_queue.push(std::move(req));
    }
}


void prefetch_urls_for(const std::vector<Track> &tracks, size_t count,
                       int cancel_search_id)
{
    if (power::low_power())
        return; // no speculative network work in low power mode
    std::vector<std::string> ids;
    ids.reserve(std::min(count, tracks.size()));
    for (const Track &t : tracks)
    {
        if (ids.size() >= count)
            break;
        if (t.id.empty() || !find_cached_audio_path(t.id).empty())
            continue;
        {
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (g_prefetched_urls.count(t.id))
                continue;
            g_prefetched_urls[t.id]; // reserve the slot (empty = in flight)
        }
        ids.push_back(t.id);
    }
    if (ids.empty())
        return;

    tasks::spawn("prefetch", [ids = std::move(ids), cancel_search_id]()
    {
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (cancel_search_id != 0 && g_state.search_id != cancel_search_id)
            {
                // Superseded: release the slots not yet resolved (completed
                // URLs stay warmed) so a future list can warm them again.
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                for (size_t j = i; j < ids.size(); ++j)
                    g_prefetched_urls.erase(ids[j]);
                return;
            }
            std::string url = fetch_stream_url_prefetch(ids[i]);
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (url.empty())
                g_prefetched_urls.erase(ids[i]); // failed: allow a retry later
            else
                g_prefetched_urls[ids[i]] = url;
        }
    });
}


void prefetch_urls_for_serial(const std::vector<Track> &tracks, size_t count,
                              const std::atomic<uint64_t> *cancel_token,
                              uint64_t token)
{
    if (power::low_power())
        return; // no speculative network work in low power mode
    std::vector<std::string> ids;
    ids.reserve(std::min(count, tracks.size()));
    for (const Track &t : tracks)
    {
        if (ids.size() >= count)
            break;
        if (t.id.empty() || !find_cached_audio_path(t.id).empty())
            continue;
        ids.push_back(t.id);
    }
    if (ids.empty())
        return;

    tasks::spawn("prefetch-serial", [ids = std::move(ids), cancel_token, token]() {
        for (const std::string &id : ids)
        {
            if (cancel_token && cancel_token->load() != token)
                return;
            {
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                if (g_prefetched_urls.count(id))
                    continue; // already warmed or another worker is on it
                g_prefetched_urls[id]; // reserve the in-flight slot
            }

            const std::string url = fetch_stream_url(id);
            {
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                if (url.empty())
                    g_prefetched_urls.erase(id);
                else
                    g_prefetched_urls[id] = url;
            }

            if (cancel_token && cancel_token->load() != token)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    });
}


std::vector<Track> upcoming_tracks()
{
    std::vector<Track> out;
    const size_t n = std::min<size_t>(3, g_state.play_queue.size());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(g_state.play_queue[i]);
    return out;
}


void prefetch_next_track()
{
    prefetch_urls_for(upcoming_tracks(), 3);
}


void predownload_next_track()
{
    if (power::low_power())
        return;
    for (const Track &t : upcoming_tracks())
        background_download(t.id);
}


std::string stream_container_ext(const std::string &url)
{
    // Case-insensitive copy: the mime type arrives raw or percent-encoded
    // and the URL's casing is not guaranteed.
    std::string u = url;
    for (char &c : u)
        c = (char)tolower((unsigned char)c);
    auto has = [&](const char *needle)
    { return u.find(needle) != std::string::npos; };

    if (has("mime=audio%2fmp4") || has("mime=audio/mp4"))
        return "m4a";
    if (has("mime=audio%2fmpeg") || has("mime=audio/mpeg"))
        return "mp3";
    if (has("mime=audio%2fwebm") || has("mime=audio/webm"))
        return "webm";
    if (has("mime=audio%2fogg") || has("mime=audio/ogg"))
        return "ogg";

    // Fall back to the YouTube audio itag when the mime param is absent.
    if (size_t p = u.find("itag="); p != std::string::npos && p + 5 < u.size())
    {
        const int itag = atoi(u.c_str() + p + 5);
        switch (itag)
        {
        case 139: // AAC in MP4
        case 140:
        case 141:
            return "m4a";
        case 170: // Vorbis/Opus in WebM
        case 171:
        case 249:
        case 250:
        case 251:
            return "webm";
        }
    }
    return "webm"; // YouTube's default audio-only muxer
}


void finalize_recording()
{
    if (g_recording_path.empty())
        return;
    g_player->set_stream_record("");
    std::string path = g_recording_path;
    std::string temp = g_recording_temp;
    std::string id = g_recording_id;
    const double expected_duration = g_recording_duration;
    g_recording_path.clear();
    g_recording_temp.clear();
    g_recording_id.clear();
    g_recording_started = false;
    g_recording_duration = 0.0;
    if (g_player->was_seeked())
    {
        // A seek outside the demuxer cache makes mpv skip stream data, so
        // the file has a hole in it. Repair in the background: the user
        // listened to (most of) the track, so it deserves a cache entry.
        remove(temp.c_str());
        background_download(id);
        return;
    }
    // Validate off-thread: mpv documents that seeks/stream switches can stop
    // the recorder outright, and a truncated or empty file must never reach
    // the cache as a "downloaded" song.
    tasks::spawn("finalize-recording", [temp, path, id, expected_duration]() mutable
                {
        const double got = probe_audio_file_duration(temp);
        if (got <= 0.0 || (expected_duration > 0.0 && got < expected_duration - 5.0))
        {
            fprintf(stderr, "Discarding recording %s: duration %.1fs (source %.1fs)\n",
                    id.c_str(), got, expected_duration);
            remove(temp.c_str());
            // mpv's recorder can stop silently mid-stream (documented for
            // seeks / stream switches); the bytes are unusable but the song
            // was heard -- fetch a clean copy in the background.
            background_download(id);
            return;
        }
        // Trust the recorded bytes over the mime/itag guess: mpv picked its
        // muxer from the extension we gave it, and a wrong container under a
        // lying extension is an unplayable cache entry.
        const size_t dot = path.find_last_of('.');
        const std::string declared = dot == std::string::npos ? "" : path.substr(dot + 1);
        const std::string real_ext = sniff_audio_container_ext(temp, declared);
        if (real_ext != declared && !real_ext.empty())
            path = recorded_audio_path(id, real_ext);
        if (path.empty() || rename(temp.c_str(), path.c_str()) != 0)
            return;
        g_downloads_dirty = true;
        finalize_recorded_audio(id, path); });
}


void discard_recording()
{
    if (g_recording_temp.empty())
        return;
    g_player->set_stream_record("");
    remove(g_recording_temp.c_str());
    const bool heard = (g_state.listen_track_id == g_recording_id &&
                        g_state.listen_seconds >= 2.0);
    if (heard)
        background_download(g_recording_id);
    g_recording_path.clear();
    g_recording_temp.clear();
    g_recording_id.clear();
    g_recording_started = false;
    g_recording_duration = 0.0;
}


void commit_play_count()
{
    if (g_state.current_track_id.empty())
        return;

    ++g_state.play_counts[g_state.current_track_id];

    PlayEvent ev;
    ev.id = g_state.current_track_id;
    ev.title = g_state.current_title;
    ev.artist = g_state.current_artist;
    ev.thumbnail_url = g_state.current_thumbnail_url;
    ev.ts = (int64_t)time(nullptr);
    g_state.play_log.push_back(ev);
    if ((int)g_state.play_log.size() > kPlayLogCap)
        g_state.play_log.erase(g_state.play_log.begin(),
                               g_state.play_log.end() - kPlayLogCap);
    g_state.stats_dirty = true;
    g_counts_dirty = true;
}


void begin_play_track(const Track &t, int index)
{
    if (t.id.empty())
        return;
    g_endless_waiting = false;
    stop_fade();
    // Invalidate resolves still in flight for the previous selection; this
    // request's generation is captured when the resolver thread is spawned.
    g_play_generation.fetch_add(1);
    if (!g_force_fresh_resolve)
        g_play_retry_count = 0;
    g_state.loading = true;
    g_loading_since = std::chrono::steady_clock::now();
    g_state.current_title = t.title;
    g_state.current_artist = t.artist;
    g_state.current_thumbnail_url = t.thumbnail_url;
    g_state.current_track_id = t.id;
    g_state.current_album_id = t.album_id;
    g_state.current_artist_id = t.artist_id;
    g_state.current_track_url = t.url; // <-- persistent
    g_state.status_text = "Loading...";
    int resolved_index = index;
    if (resolved_index < 0)
    {
        // Queue entries do not carry a source index. Re-find the row when the
        // active list still contains this track so list highlighting survives
        // a skip.
        auto playlist = get_playlist(g_state);
        if (playlist)
        {
            for (int i = 0; i < (int)playlist->size(); ++i)
            {
                if ((*playlist)[i].id == t.id)
                {
                    resolved_index = i;
                    break;
                }
            }
        }
    }
    g_state.current_track_index = resolved_index;

    g_state.pending_manual_play = true;
    g_state.pending_play_url = t.url;
    g_state.pending_play_title = t.title;
    g_state.pending_play_artist = t.artist;
    g_state.pending_play_thumbnail_url = t.thumbnail_url;
    g_state.pending_video_id = t.id;
    g_state.pending_video_url = t.url;
}


void replay_current_track()
{
    Track t;
    t.id = g_state.current_track_id;
    t.title = g_state.current_title;
    t.artist = g_state.current_artist;
    t.artist_id = g_state.current_artist_id;
    t.album_id = g_state.current_album_id;
    t.thumbnail_url = g_state.current_thumbnail_url;
    t.url = g_state.current_track_url;
    begin_play_track(t, g_state.current_track_index);
}



bool maybe_start_endless_refill()
{
    if (!g_state.settings.endless_radio || g_state.settings.repeat_mode != 0 ||
        g_state.settings.shuffle_enabled)
        return false;
    if (g_state.current_track_id.empty())
        return false;
    if (g_endless_seed != g_state.current_track_id)
    {
        g_endless_seed = g_state.current_track_id;
        g_endless_attempted = false;
    }
    if (g_state.endless_loading.load())
        return true;
    if (g_endless_attempted)
        return false;
    if (g_state.play_queue.size() >= 100)
        return false;

    const int remaining = (int)g_state.play_queue.size();
    if (remaining > 3)
        return false;

    g_endless_attempted = true;
    g_state.endless_loading = true;
    const std::string seed = g_state.current_track_id;
    tasks::spawn("endless-radio", [seed]()
                {
        std::vector<Track> results;
        try {
            results = fetch_radio(seed);
        } catch (...) {}
        std::atomic_store(&g_state.endless_results,
                          std::make_shared<const std::vector<Track>>(std::move(results)));
        g_state.endless_ready = true;
        g_state.endless_loading = false;
        g_state.repaint_requested = true; });
    return true;
}


void consume_endless_results()
{
    if (!g_state.endless_ready.exchange(false))
        return;
    auto results = std::atomic_load(&g_state.endless_results);
    bool appended = false;

    if (g_state.settings.endless_radio && results)
    {
        std::unordered_set<std::string> seen;
        if (!g_state.current_track_id.empty())
            seen.insert(g_state.current_track_id);
        for (const auto &t : g_state.play_queue)
            if (!t.id.empty())
                seen.insert(t.id);
        for (const auto &t : g_state.playback_history)
            if (!t.id.empty())
                seen.insert(t.id);

        for (const auto &t : *results)
        {
            if (t.id.empty() || seen.count(t.id))
                continue;
            if (g_state.play_queue.size() >= 100)
                break;
            seen.insert(t.id);
            g_state.play_queue.push_back(t);
            appended = true;
        }
        g_state.endless_loading = false;
    }

    if (appended)
    {
        g_state.queue_dirty = true;
        g_state.repaint_requested = true;
        prefetch_next_track();
    }

    if (g_endless_waiting)
    {
        g_endless_waiting = false;
        if (appended)
        {
            if (!advance_playback())
                g_state.status_text = "Ready.";
        }
        else if (!g_state.endless_loading.load())
        {
            g_state.status_text = "Ready.";
        }
    }
}


void refill_queue_from_cycle()
{
    if (g_state.queue_cycle.empty())
        return;
    g_state.play_queue = g_state.queue_cycle;
    if (g_state.settings.shuffle_enabled && g_state.play_queue.size() > 1)
    {
        static std::mt19937 rng(
            (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::shuffle(g_state.play_queue.begin(), g_state.play_queue.end(), rng);
    }
    g_state.queue_dirty = true;
}


bool select_next_track(bool allow_refill, bool consume, NextPick &out)
{
    if (g_state.play_queue.empty() && allow_refill &&
        g_state.settings.repeat_mode != 0)
        refill_queue_from_cycle();

    if (g_state.play_queue.empty())
        return false;

    out.track = g_state.play_queue.front();
    if (consume)
    {
        g_state.play_queue.erase(g_state.play_queue.begin());
        g_state.queue_dirty = true;
    }
    return true;
}


void adopt_fade_track(const Track &consumed, int consumed_index)
{
    g_state.loading = false;
    // Keep the outgoing track around for the first half of the fade. Lyrics
    // and their metadata stay in sync with the bottom bar's midpoint switch.
    g_state.crossfade_from_track = Track{};
    g_state.crossfade_from_track.id = g_state.current_track_id;
    g_state.crossfade_from_track.title = g_state.current_title;
    g_state.crossfade_from_track.artist = g_state.current_artist;
    g_state.crossfade_from_track.thumbnail_url = g_state.current_thumbnail_url;
    g_state.crossfade_from_track.url = g_state.current_track_url;
    g_state.crossfade_from_track.album_id = g_state.current_album_id;
    g_state.crossfade_from_track.artist_id = g_state.current_artist_id;
    g_state.current_title = consumed.title;
    g_state.current_artist = consumed.artist;
    g_state.current_thumbnail_url = consumed.thumbnail_url;
    g_state.current_track_id = consumed.id;
    g_state.current_album_id = consumed.album_id;
    g_state.current_artist_id = consumed.artist_id;
    g_state.current_track_url = consumed.url;
    g_state.status_text = "Playing";
    g_state.current_track_index = consumed_index;
    g_state.listen_track_id = consumed.id;
    g_state.listen_seconds = 0.0;
    g_state.listen_counted = false;
    // Lyrics stay on the outgoing track until the visual midpoint; the render
    // loop queues the incoming lyrics when crossfade_progress crosses 0.5.

    // History/notification bookkeeping, mirroring the main "start" path.
    g_history_dirty = true;
    {
        Track history_entry;
        history_entry.id = consumed.id;
        history_entry.title = consumed.title;
        history_entry.artist = consumed.artist;
        history_entry.thumbnail_url = consumed.thumbnail_url;
        history_entry.url = consumed.url;
        history_entry.duration_sec = consumed.duration_sec;
        for (auto it = g_state.playback_history.begin();
             it != g_state.playback_history.end(); ++it)
        {
            if (it->id == history_entry.id)
            {
                g_state.playback_history.erase(it);
                break;
            }
        }
        g_state.playback_history.insert(g_state.playback_history.begin(), history_entry);
        if (g_state.playback_history.size() > 200)
            g_state.playback_history.resize(200);
    }
    if (g_mpris)
    {
        g_mpris->update_metadata(consumed, (double)consumed.duration_sec);
        g_mpris->update_playback_status(true);
        update_discord_presence();
    }
    {
        std::string title = consumed.title.empty() ? "Unknown Track" : consumed.title;
        std::string artist = consumed.artist.empty() ? "Unknown Artist" : consumed.artist;
        // Stat-only: never a synchronous curl on the main thread (the async
        // pipeline fetches the art; the notification falls back to a generic
        // icon until it lands).
        std::string thumb_path = thumbnail_is_cached(consumed.thumbnail_url)
                                     ? get_cached_thumbnail_path(consumed.thumbnail_url)
                                     : "";
        if (thumb_path.empty())
            thumb_path = "audio-x-generic";
        show_notification(title, "by " + artist, thumb_path);
    }
}


bool can_start_manual_crossfade(const Track &t)
{
    if (!g_state.settings.skip_crossfade_enabled)
        return false;
    if (g_state.settings.skip_crossfade_duration <= 0.0f)
        return false;
    if (!g_state.playing.load() || !g_player || !g_player->is_playing())
        return false;
    if (t.id.empty())
        return false;
    return true;
}


void promote_fade_player_to_main()
{
    if (!g_fade_player)
        return;
    g_cf.pending = false;
    g_cf.active = false;
    g_cf.manual = false;

    g_player.swap(g_fade_player);
    g_player_raw = g_player.get();
    // Callbacks live on the objects, not the roles: move them with the swap.
    g_player->set_event_callback(g_main_event_cb);
    g_fade_player->set_event_callback(g_fade_event_cb);
    g_player->set_volume(g_cf.user_volume);
    g_fade_player->set_volume(0);
    g_fade_player->set_stream_record("");
    g_fade_player->stop();

    g_cf.track = Track{};
    g_cf.index = -1;
    g_state.playing = true;
    if (!g_state.current_track_id.empty() &&
        g_state.lyrics_requested_id != g_state.current_track_id)
    {
        g_state.lyrics_requested_id = g_state.current_track_id;
        g_state.lyrics_dirty = true;
    }
    g_state.repaint_requested = true;
}


void finish_manual_crossfade()
{
    if (!g_fade_player)
        return;
    promote_fade_player_to_main();
    g_state.loading = false;
    g_state.status_text = "Playing";

    prefetch_next_track();
    predownload_next_track();
    maybe_start_endless_refill();
}


void settle_inflight_crossfade()
{
    if (!g_fade_player)
        return;
    if (g_cf.active.load())
    {
        // The faded-in track is already audible, so make it the outgoing
        // track for the new selection instead of letting the helper linger.
        discard_recording();
        promote_fade_player_to_main();
    }
    else if (g_cf.pending.load())
    {
        // The incoming track never became audible: cancel it and keep the
        // current main player, which can switch to the new selection.
        stop_fade();
    }
}


void start_manual_crossfade(const Track &t, int index,
                                   const std::string &cached_path, double seconds)
{
    // Restart semantics: a second Next during a fade cancels the running one
    // and starts a fresh fade toward the newly picked track.
    stop_fade();
    // The outgoing track is being skipped, so drop its partial recording now
    // (while the listen timer still refers to it -- discard_recording repairs
    // it in the background when enough was heard).
    discard_recording();
    // Invalidate resolves still in flight for an earlier selection.
    g_play_generation.fetch_add(1);
    g_state.pending_manual_play = false;
    g_state.pending_play_url.clear();

    g_cf.track = t;
    g_cf.index = index;
    adopt_fade_track(t, index);

    g_cf.user_volume = (float)g_player->get_property_double("volume");
    if (g_cf.user_volume <= 0.0f)
        g_cf.user_volume = 100.0f;
    g_fade_player->set_stream_record("");
    g_fade_player->set_volume(0);
    g_cf.seconds = seconds;
    g_cf.manual = true;
    g_cf.pending = true;
    g_cf.start = std::chrono::steady_clock::now();
    g_fade_player->play(cached_path);

    prefetch_next_track();
    predownload_next_track();
    maybe_start_endless_refill();
}


bool try_start_manual_crossfade(const Track &t, int index)
{
    if (!can_start_manual_crossfade(t))
        return false;

    // Don't start a fade that the outgoing file would finish before the
    // incoming one reaches full volume; an instant switch is cleaner there.
    const double dur = g_player->cached_duration();
    const double pos = g_player->cached_time_pos();
    if (dur > 0.0 && pos >= 0.0 && dur - pos < 0.5)
        return false;

    const std::string cached = find_cached_audio_path(t.id);
    if (cached.empty())
        return false;

    double cf = (double)g_state.settings.skip_crossfade_duration;
    if (t.duration_sec > 0)
        cf = std::min(cf, t.duration_sec * 0.5);
    if (dur > 0.0 && pos >= 0.0)
        cf = std::min(cf, dur - pos);
    if (cf < 0.1)
        cf = 0.1;

    start_manual_crossfade(t, index, cached, cf);
    return true;
}


void maybe_start_crossfade()
{
    // Keep mpv's crossfade option in sync with the settings slider (unused
    // by mpv 0.41+, which has no crossfade support, but harmless).
    if (g_cf.applied_duration != g_state.settings.crossfade_duration)
    {
        g_cf.applied_duration = g_state.settings.crossfade_duration;
        g_player->set_crossfade(g_cf.applied_duration);
    }

    // Playback speed: applied to BOTH instances so it survives the crossfade
    // role swap.
    static float g_applied_speed = -1.0f;
    if (g_applied_speed != g_state.settings.playback_speed)
    {
        g_applied_speed = g_state.settings.playback_speed;
        g_player->set_speed((double)g_applied_speed);
        g_fade_player->set_speed((double)g_applied_speed);
    }

    // Audio filters (normalization + EQ): one chain on both instances.
    static std::string g_applied_af;
    static bool g_af_first = true;
    std::string af = crossfade::audio_filter(g_state.settings);
    if (g_af_first || af != g_applied_af)
    {
        g_af_first = false;
        g_applied_af = af;
        g_player->set_af(af);
        g_fade_player->set_af(af);
    }

    // Manual skip fades are timer-driven: the outgoing file does not reach
    // EOF, so promote the helper once the ramp has run its course.
    if (g_cf.manual.load())
    {
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - g_cf.start)
                                   .count();
        switch (crossfade::plan_manual(g_cf.manual, g_cf.pending, g_cf.active, elapsed,
                                       g_cf.seconds))
        {
        case crossfade::ManualAction::Cancel:
            g_cf.manual = false;
            break;
        case crossfade::ManualAction::Finish:
            finish_manual_crossfade();
            return;
        case crossfade::ManualAction::StartFailed:
        {
            // The cached file never started (vanished, decoder error): fall
            // back to the normal switch instead of leaving silence.
            Track target = g_cf.track;
            const int target_index = g_cf.index;
            g_cf.track = Track{};
            g_cf.index = -1;
            stop_fade();
            if (!target.id.empty())
                begin_play_track(target, target_index);
            return;
        }
        case crossfade::ManualAction::None:
            break;
        }
    }

    // Drive the fade: perceptually even, constant-power ramp on the two
    // players, and mirror the app's play/pause intent onto the helper.
    // The ramp runs at ~20 Hz -- plenty for a smooth loudness glide; writing
    // mpv properties on every 16 ms frame only burns wakeups.
    if ((g_cf.pending || g_cf.active) && g_fade_player)
    {
        static std::chrono::steady_clock::time_point s_last_ramp;
        const auto ramp_now = std::chrono::steady_clock::now();
        if (ramp_now - s_last_ramp < std::chrono::milliseconds(50))
            return;
        s_last_ramp = ramp_now;
        const double cf = std::max(g_cf.seconds, 0.1);
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - g_cf.start)
                                   .count();
        const double t = std::min(elapsed / cf, 1.0);
        const crossfade::Gains g = crossfade::gains(t);
        g_fade_player->set_volume(g.incoming * g_cf.user_volume);
        g_player->set_volume(g.outgoing * g_cf.user_volume);
        g_fade_player->set_paused(!g_state.playing.load());
    }

    if (g_state.settings.crossfade_duration <= 0.0f)
        return;
    if (g_cf.pending || g_cf.active)
        return;
    if (!g_state.playing.load() || g_state.loading)
        return;
    if (g_state.settings.repeat_mode == 2)
        return; // repeat-one replays via replay_current_track()

    const double dur = g_player->cached_duration();
    if (dur <= 0.0)
        return;
    // Never spend more than half the track on the fade: a 20 s setting on a
    // 15 s interlude would otherwise start the next track straight away.
    const double cf = std::min((double)g_state.settings.crossfade_duration, dur * 0.5);
    if (cf <= 0.0)
        return;
    const double pos = g_player->cached_time_pos();
    if (pos <= 0.0 || dur - pos > cf)
        return;

    NextPick pick;
    if (!select_next_track(false, false, pick))
        return;
    // Crossfade only plays the next track from the local cache (the
    // predownload worker usually has it ready): playing a resolved remote
    // URL here would bypass the stream-recording machinery and risk the
    // 403s of stale googlevideo links. When it isn't cached yet, the
    // normal advance at end-of-file handles it without a fade.
    std::string src = find_cached_audio_path(pick.track.id);
    crossfade::NaturalInputs fade_inputs;
    fade_inputs.crossfade_duration = g_state.settings.crossfade_duration;
    fade_inputs.track_duration = dur;
    fade_inputs.position = pos;
    fade_inputs.playing = g_state.playing;
    fade_inputs.loading = g_state.loading;
    fade_inputs.already_fading = g_cf.pending || g_cf.active;
    fade_inputs.repeat_one = g_state.settings.repeat_mode == 2;
    fade_inputs.target_cached = !src.empty();
    if (!crossfade::should_start_natural(fade_inputs))
        return;

    // Consume exactly the peeked pick.
    if (g_state.play_queue.empty() ||
        g_state.play_queue.front().id != pick.track.id)
        return; // queue changed underneath us
    g_state.play_queue.erase(g_state.play_queue.begin());
    g_state.queue_dirty = true;
    const Track &consumed = pick.track;
    const int consumed_index = -1;

    g_cf.track = consumed;
    g_cf.index = consumed_index;

    adopt_fade_track(consumed, consumed_index);

    // Start the next track silently on the helper player; the per-frame
    // volume ramp below fades the two into each other.
    g_cf.user_volume = (float)g_player->get_property_double("volume");
    if (g_cf.user_volume <= 0.0f)
        g_cf.user_volume = 100.0f;
    g_fade_player->set_stream_record("");
    g_fade_player->set_volume(0);
    g_cf.seconds = cf;
    g_cf.manual = false; // natural end-of-track fade
    g_cf.pending = true;
    g_cf.start = std::chrono::steady_clock::now();
    g_fade_player->play(src);

    prefetch_next_track();
    predownload_next_track();
    maybe_start_endless_refill();
}


bool advance_playback(bool manual)
{
    NextPick pick;
    if (select_next_track(true, true, pick))
    {
        if (manual && try_start_manual_crossfade(pick.track, -1))
            return true;
        begin_play_track(pick.track, -1);
        return true;
    }
    return false;
}


void handle_player_event(const std::string &event, MprisPlayer *mpris)
{
    if (event == "start") {
        static bool logged_first_audio = false;
        if (!logged_first_audio)
        {
            DLOG("boot: first audio at %lld", DNOW());
            logged_first_audio = true;
        }
        g_state.loading = false;
        g_state.status_text = "Playing";
        g_state.repaint_requested = true;

        bool was_manual = g_state.pending_manual_play.exchange(false);
        if (was_manual) {
            // UI already updated for the track we asked for.
        } else {
            // Unexpected auto-start: the metadata set by begin_play_track()
            // is the closest guess and must be kept — do NOT advance the
            // index here. A stale (superseded) resolve playing after a
            // newer one already started used to land in this branch and
            // overwrite the indicator with the *next* list entry while a
            // different song was audible.
            g_state.playing = true;
        }

        // Start timing this listen. The play count / stats entry is
        // only committed once 10s of actual playback have elapsed (see
        // the main loop), so skipped tracks don't count.
        g_state.listen_track_id = g_state.current_track_id;
        g_state.listen_seconds = 0.0;
        g_state.listen_counted = false;

        // The track now under recording reports its duration from here
        // on; finalize_recording() validates the finished file against
        // it (may still be 0 until the header is parsed -> polled in the
        // main loop until it shows up).
        if (!g_recording_path.empty())
        {
            g_recording_started = true;
            const double d = g_player->get_property_double("duration");
            if (d > 0.0)
                g_recording_duration = d;
        }

        if (g_state.session_restore_pending) {
            g_state.session_restore_pending = false;
            const double restore_pos = g_state.session_restore.position;
            if (restore_pos > 0)
            {
                // The stream just started, so the cache almost certainly
                // doesn't cover the saved position yet; seeking there
                // now would skip data and break this track's recording.
                // Wait (bounded) until the target is inside the cached
                // region, then jump without hurting the recording.
                if (g_recording_path.empty() || g_player->is_position_cached(restore_pos))
                    g_player->seek(restore_pos);
                else
                {
                    g_pending_restore_seek = restore_pos;
                    g_pending_restore_seek_id = g_state.current_track_id;
                    g_pending_restore_seek_gen = g_player->seek_generation();
                    g_pending_restore_seek_deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(15);
                }
            }
            int vol = g_state.session_restore.volume;
            if (vol >= 0 && vol <= 100)
                g_player->set_volume(vol);
        }
        // Make mpv match the app's play/pause intent. The saved state
        // was folded into g_state.playing when the track was queued, and
        // any play/pause the user pressed while it was still resolving
        // is honored here, so a single click always sticks.
        g_player->set_paused(!g_state.playing.load());
        if (!g_state.playing)
            g_state.status_text = "Paused";

        Track current_track;
        current_track.id = g_state.current_track_id;
        current_track.title = g_state.current_title;
        current_track.artist = g_state.current_artist;
        current_track.thumbnail_url = g_state.current_thumbnail_url;
        current_track.url = g_state.current_track_url;
        double duration = g_player->get_property_double("duration");
        mpris->update_metadata(current_track, duration);
        mpris->update_playback_status(g_state.playing);
        update_discord_presence();
        double position = g_player->get_property_double("time-pos");
        mpris->update_position(position);
        double vol = g_player->get_property_double("volume");
        mpris->update_volume(vol / 100.0);

        std::string title = g_state.current_title;
        std::string artist = g_state.current_artist;
        if (title.empty()) title = "Unknown Track";
        if (artist.empty()) artist = "Unknown Artist";
        std::string thumb_path = thumbnail_is_cached(g_state.current_thumbnail_url)
                                     ? get_cached_thumbnail_path(g_state.current_thumbnail_url)
                                     : "";
        if (thumb_path.empty()) thumb_path = "audio-x-generic";
        show_notification(title, "by " + artist, thumb_path);

        // ---- Record playback history ----
        g_history_dirty = true;
        {
            Track history_entry;
            history_entry.id = g_state.current_track_id;
            history_entry.title = g_state.current_title;
            history_entry.artist = g_state.current_artist;
            history_entry.thumbnail_url = g_state.current_thumbnail_url;
            history_entry.url = g_state.current_track_url;
            history_entry.duration_sec = (int)duration;
            // Insert at front (most-recent-first). Drop an earlier
            // entry for the same track first: history keeps one row per
            // track, so repeats don't stack duplicates.
            for (auto it = g_state.playback_history.begin();
                 it != g_state.playback_history.end(); ++it)
            {
                if (it->id == history_entry.id)
                {
                    g_state.playback_history.erase(it);
                    break;
                }
            }
            g_state.playback_history.insert(g_state.playback_history.begin(), history_entry);
            // Cap at 200 entries
            if (g_state.playback_history.size() > 200)
                g_state.playback_history.resize(200);
        }

        prefetch_next_track();
        predownload_next_track();

        // Mark the track for a lyrics lookup. The actual fetch happens
        // in the main loop and only while the lyrics panel is open, so
        // playing with the panel closed costs nothing.
        g_state.lyrics_requested_id = g_state.current_track_id;
        g_state.lyrics_dirty = true;

        maybe_start_endless_refill();

    } else if (event == "end") {
        DLOG("EVENT end");
        if (g_cf.active.exchange(false)) {
            // The helper player already faded the next track in; this
            // EOF is just the outgoing file reaching its end. Finalize
            // its recording, then promote the helper to be the main
            // player (roles swap, so the next crossfade reuses the
            // freshly retired instance).
            finalize_recording();
            g_cf.manual = false;
            g_player.swap(g_fade_player);
            g_player_raw = g_player.get();
            // The callbacks live on the MpvPlayer objects, not the
            // roles: move them with the swap or the new main player
            // would keep swallowing its own end-of-file events.
            g_player->set_event_callback(g_main_event_cb);
            g_fade_player->set_event_callback(g_fade_event_cb);
            g_player->set_volume(g_cf.user_volume);
            g_fade_player->set_volume(0);
            g_cf.track = Track{};
            g_cf.index = -1;
            if (!g_state.current_track_id.empty() &&
                g_state.lyrics_requested_id != g_state.current_track_id)
            {
                g_state.lyrics_requested_id = g_state.current_track_id;
                g_state.lyrics_dirty = true;
            }
            g_state.repaint_requested = true;
            if (g_state.sleep_at_end_of_track)
            {
                // "Stop after this track": the next track already faded
                // in, so pause it immediately.
                g_state.sleep_at_end_of_track = false;
                g_player->set_paused(true);
                g_state.playing = false;
                mpris->update_playback_status(false);
                update_discord_presence();
                ui_notify(UiNotifyType::Info, "Sleep timer",
                          "Paused at end of track.");
            }
            return;
        }
        // Fade was issued but the helper never started (e.g. its file
        // vanished): cancel it so the normal advance doesn't overlap.
        if (g_cf.pending.exchange(false))
        {
            const bool manual = g_cf.manual.exchange(false);
            Track target = g_cf.track;
            const int target_index = g_cf.index;
            g_fade_player->set_stream_record("");
            g_fade_player->stop();
            if (manual)
            {
                // The outgoing file ended before the helper started:
                // finalize it and start the target through the normal
                // path so the transition isn't lost.
                g_cf.track = Track{};
                g_cf.index = -1;
                finalize_recording();
                if (!target.id.empty())
                    begin_play_track(target, target_index);
                return;
            }
        }
        // A manual selection is already pending (its resolve is still
        // running): let it take over instead of advancing. Advancing
        // here is what makes a slow non-cached resolve finish after
        // this EOF and overwrite the track that was actually picked.
        if (g_state.pending_manual_play.load() || !g_state.pending_play_url.empty())
        {
            finalize_recording();
            return;
        }
        g_state.playing = false;
        g_state.repaint_requested = true;
        mpris->update_playback_status(false);
        update_discord_presence();
        // Track finished on its own: keep the stream recording, then
        // advance to the next entry explicitly (so the UI metadata
        // always matches what is actually playing).
        finalize_recording();
        if (g_state.sleep_at_end_of_track)
        {
            g_state.sleep_at_end_of_track = false;
            ui_notify(UiNotifyType::Info, "Sleep timer",
                      "Paused at end of track.");
        }
        else if (g_state.settings.repeat_mode == 2)
        {
            replay_current_track();
        }
        else if (!advance_playback())
        {
            if (g_state.settings.endless_radio && g_state.settings.repeat_mode == 0)
            {
                g_endless_waiting = true;
                if (maybe_start_endless_refill() || g_state.endless_ready.load())
                    g_state.status_text = "Loading endless radio...";
                else
                {
                    g_endless_waiting = false;
                    g_state.status_text = "Ready.";
                }
            }
            else
            {
                g_state.status_text = "Ready.";
            }
        }
    } else if (event == "stop" || event.rfind("error", 0) == 0) {
        DLOG("EVENT %s", event.c_str());
        stop_fade();
        const bool started_playing = (g_state.listen_track_id == g_state.current_track_id &&
                                      g_state.listen_seconds >= 2.0);
        const bool load_failed = event.rfind("error", 0) == 0 &&
                                 !g_state.current_track_id.empty() &&
                                 find_cached_audio_path(g_state.current_track_id).empty() &&
                                 !started_playing;
        if (load_failed && g_play_retry_count < 2)
        {
            ++g_play_retry_count;
            g_force_fresh_resolve = true;
            g_state.status_text = "Retrying stream...";
            g_state.repaint_requested = true;
            replay_current_track();
        }
        else
        {
            if (event.rfind("error", 0) == 0)
            {
                // Prefer the classified reason from the failed resolve
                // (auth / network / backend) over a generic message.
                std::string hint;
                {
                    std::lock_guard<std::mutex> lock(g_play_fail_mutex);
                    hint = std::move(g_play_fail_hint);
                    g_play_fail_hint.clear();
                }
                ui_notify(UiNotifyType::Warning, "Playback failed",
                          hint.empty()
                              ? (g_state.current_title.empty()
                                     ? "The stream could not be loaded. Try again in a moment."
                                     : "Couldn't play \"" + g_state.current_title +
                                           "\". Check your connection or try again.")
                              : hint,
                          8.0f);
            }
            g_state.loading = false;
            g_state.playing = false;
            g_state.pending_manual_play = false;
            g_state.status_text = "Ready.";
            g_state.repaint_requested = true;
            g_state.listen_track_id.clear();
            // NB: don't discard the recording here. A manual skip fires
            // this *after* the next track has already begun recording, so
            // the consume path / shutdown are responsible for cleanup.
            mpris->update_playback_status(false);
            update_discord_presence();
        }
}
}

