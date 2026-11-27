#include "app_internal.h"
#include "tunables.h"

#include "ytdlp.h"
#include "mpvplayer.h"
#include "local_audio.h"
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
#include "party_queue.h"
#include "video_surface.h"

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

// Cover URL for playback metadata. A list row can draw an album/artist image
// as a fallback, but the queue entry handed to auto-advance has no such
// context: synthesize the video still rather than leave the bottom bar, the
// lyrics header and the system media notification without artwork.
static std::string playback_thumb_url(const Track &t)
{
    if (!t.thumbnail_url.empty())
        return t.thumbnail_url;
    if (!t.id.empty() && !is_local_track_id(t.id))
        return "https://img.youtube.com/vi/" + t.id + "/hqdefault.jpg";
    return std::string();
}

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

    // Guest party requests are not part of the host's source list. A source
    // rebuild happens whenever the host starts a track, so clearing the queue
    // outright used to silently drop every guest request -- the party queue
    // then "jumped" back to the host playlist. Carry them across and re-insert
    // them (fairly ordered) ahead of the freshly materialized host tracks.
    std::vector<Track> guest_items;
    guest_items.reserve(g_state.play_queue.size());
    for (Track &t : g_state.play_queue)
        if (t.from_party)
            guest_items.push_back(std::move(t));

    g_state.play_queue.clear();
    g_state.queue_cycle.clear();

    auto playlist = get_playlist(g_state);
    if (!playlist || playlist->empty())
    {
        // The host's source is gone, but the party queue must survive.
        for (Track &t : guest_items)
            party_queue::insert_fair(g_state.play_queue, std::move(t));
        g_state.queue_dirty = true;
        g_state.repaint_requested = true;
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

    // Put the guest requests back in front of the host list, preserving the
    // round-robin order insert_fair() assigned when they were requested.
    for (Track &t : guest_items)
        party_queue::insert_fair(g_state.play_queue, std::move(t));

    g_state.queue_dirty = true;
    g_state.repaint_requested = true;
}


void resolve_audio(const std::string &video_id, const std::string &video_url,
                          const std::string &title, const std::string &artist,
                          const std::string &thumb, int expected_duration_sec,
                          bool fresh, std::uint64_t gen)
{
    (void)title;
    (void)artist;
    (void)thumb;
    PlayRequest req;
    req.id = video_id;
    req.gen = gen;
    req.expected_duration_sec = expected_duration_sec;
    if (is_local_track_id(video_id))
    {
        // A file on disk: play it directly and never touch the network or the
        // stream-recording machinery.
        req.path = video_url;
        req.remote = false;
        std::lock_guard<std::mutex> lock(g_play_queue_mutex);
        g_play_queue.push(std::move(req));
        return;
    }
    // A Downloads video row carries its local file in `video_url`. Play that
    // file directly (offline) instead of re-resolving the stream.
    if (!video_url.empty() && video_url.find("://") == std::string::npos)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(video_url, ec))
        {
            req.path = video_url;
            req.remote = false;
            std::lock_guard<std::mutex> lock(g_play_queue_mutex);
            g_play_queue.push(std::move(req));
            return;
        }
    }
    req.path = find_cached_audio_path(video_id);
    req.remote = false;
    // A cache entry is only usable if its length agrees with the track
    // metadata. A wrong song can otherwise sit under the right id forever
    // (mpv reports that file's own duration, so stream validation alone
    // cannot catch it) and be replayed on every play.
    if (!req.path.empty() && expected_duration_sec > 0)
    {
        const double cached_duration = probe_audio_file_duration(req.path);
        const double meta_duration = (double)expected_duration_sec;
        if (cached_duration > 0.0 &&
            std::abs(cached_duration - meta_duration) > std::max(10.0, meta_duration * 0.2))
        {
            fprintf(stderr, "Quarantining cached audio for %s: %.1fs vs metadata %ds\n",
                    video_id.c_str(), cached_duration, expected_duration_sec);
            std::error_code ec;
            std::filesystem::rename(req.path, req.path + ".wrong-duration", ec);
            if (ec)
                std::filesystem::remove(req.path, ec);
            std::filesystem::remove(req.path + ".meta.json", ec);
            req.path.clear();
        }
    }
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
                       int cancel_search_id, size_t batch)
{
    if (power::low_power())
        return; // no speculative network work in low power mode
    std::vector<std::string> ids;
    ids.reserve(std::min(count, tracks.size()));
    for (const Track &t : tracks)
    {
        if (ids.size() >= count)
            break;
        if (t.id.empty() || is_local_track_id(t.id) ||
            !find_cached_audio_path(t.id).empty())
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

    tasks::spawn("prefetch", [ids = std::move(ids), cancel_search_id, batch]()
    {
        // Resolve in backend-parallel batches. One command per batch keeps the
        // pipe from serializing four extractions behind each other; between
        // batches we can still abandon a superseded search. Background batches
        // request a small size so a lyrics request can slip in between them.
        const size_t kBatch = batch > 0
                                  ? batch
                                  : (size_t)tunable_int("playback.prefetch_batch");
        for (size_t start = 0; start < ids.size(); start += kBatch)
        {
            if (cancel_search_id != 0 && g_state.search_id != cancel_search_id)
            {
                // Superseded: release the slots not yet resolved (completed
                // URLs stay warmed) so a future list can warm them again.
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                for (size_t j = start; j < ids.size(); ++j)
                {
                    auto it = g_prefetched_urls.find(ids[j]);
                    if (it != g_prefetched_urls.end() && it->second.empty())
                        g_prefetched_urls.erase(it);
                }
                return;
            }
            std::vector<std::string> batch(ids.begin() + start,
                                           ids.begin() + std::min(ids.size(), start + kBatch));
            auto urls = fetch_stream_urls_prefetch(batch, /*background=*/true);
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            for (const std::string &id : batch)
            {
                auto it = urls.find(id);
                if (it != urls.end() && !it->second.empty())
                    g_prefetched_urls[id] = it->second;
                else
                {
                    // Failed: allow a retry later, but never drop a URL the
                    // priority hover worker warmed in the meantime.
                    auto cur = g_prefetched_urls.find(id);
                    if (cur != g_prefetched_urls.end() && cur->second.empty())
                        g_prefetched_urls.erase(cur);
                }
            }
        }
    });
}

// Hover warming: resolving takes ~1.5-2 s, so the moment the pointer rests on
// a row we can have its URL ready before the click lands. One at a time, and
// only for rows not already reserved by the list prefetch.
static std::atomic<bool> g_hover_prefetch_active{false};
static std::mutex g_hover_pending_mutex;
static std::string g_hover_pending;
static bool g_hover_pending_set = false;

void prefetch_hovered_track(const std::string &video_id);

static bool prefetch_url_ready(const std::string &video_id)
{
    std::lock_guard<std::mutex> lock(g_prefetch_mutex);
    auto it = g_prefetched_urls.find(video_id);
    return it != g_prefetched_urls.end() && !it->second.empty();
}

static void finish_hover_cycle()
{
    std::string next;
    {
        std::lock_guard<std::mutex> lock(g_hover_pending_mutex);
        if (g_hover_pending_set)
        {
            next = std::move(g_hover_pending);
            g_hover_pending.clear();
            g_hover_pending_set = false;
        }
    }
    if (!next.empty())
    {
        // Re-enter through the public entry so its checks run on the latest id.
        g_hover_prefetch_active.store(false);
        prefetch_hovered_track(next);
    }
    else
    {
        g_hover_prefetch_active.store(false);
    }
}

static void start_hover_prefetch(const std::string &video_id)
{
    tasks::spawn("hover-prefetch", [video_id]()
    {
        auto urls = fetch_stream_urls_prefetch(std::vector<std::string>{video_id});
        {
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            auto it = urls.find(video_id);
            if (it != urls.end() && !it->second.empty())
                g_prefetched_urls[video_id] = it->second;
            else
            {
                // Keep a URL another channel may have warmed meanwhile.
                auto cur = g_prefetched_urls.find(video_id);
                if (cur != g_prefetched_urls.end() && cur->second.empty())
                    g_prefetched_urls.erase(cur);
            }
        }
        g_state.repaint_requested = true;
        finish_hover_cycle();
    });
}

void prefetch_hovered_track(const std::string &video_id)
{
    if (video_id.empty() || power::low_power() || is_local_track_id(video_id))
        return;
    if (!find_cached_audio_path(video_id).empty())
        return;
    if (prefetch_url_ready(video_id))
        return;
    if (g_hover_prefetch_active.exchange(true))
    {
        // One resolve at a time on the priority channel; remember the latest
        // request so the row hovered while busy still gets warmed.
        std::lock_guard<std::mutex> lock(g_hover_pending_mutex);
        g_hover_pending = video_id;
        g_hover_pending_set = true;
        return;
    }
    start_hover_prefetch(video_id);
}

// "Prefetch the world": warm the visible list (not just the hovered row).
// Runs on the meta channel so it cannot delay a click or a hover warmup.
void prefetch_visible_list_if_aggressive()
{
    if (!g_state.settings.aggressive_prefetch || power::low_power())
        return;
    TrackListPtr list = get_playlist(g_state);
    if (!list || list->empty())
        return;
    // Only when the page/list actually changes: the per-id reservation map
    // would make repeats cheap, but walking a 1000-row list every frame is not.
    static uint64_t s_sig = 0;
    uint64_t sig = (uint64_t)(int)g_state.current_tab * 1000003u;
    sig ^= (uint64_t)list->size() * 7919u;
    sig ^= (uint64_t)(g_state.selected_playlist_index + 1) << 17;
    if (!list->front().id.empty())
        sig ^= std::hash<std::string>{}(list->front().id);
    if (list->size() > 1 && !list->back().id.empty())
        sig ^= std::hash<std::string>{}(list->back().id) << 1;
    if (sig == s_sig)
        return;
    s_sig = sig;
    const size_t count =
        (size_t)std::max(0, tunable_int("prefetch.aggressive_count"));
    if (count > 0)
    {
        const size_t batch = (size_t)std::max(1, tunable_int("prefetch.aggressive_batch"));
        prefetch_urls_for(*list, count, 0, batch);
    }
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
        if (t.id.empty() || is_local_track_id(t.id) ||
            !find_cached_audio_path(t.id).empty())
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
                {
                    auto cur = g_prefetched_urls.find(id);
                    if (cur != g_prefetched_urls.end() && cur->second.empty())
                        g_prefetched_urls.erase(cur);
                }
                else
                    g_prefetched_urls[id] = url;
            }

            if (cancel_token && cancel_token->load() != token)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(tunable_int("playback.prefetch_serial_gap_ms")));
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
    g_recording_expected_duration = 0;
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
    const int expected_meta_duration = g_recording_expected_duration;
    tasks::spawn("finalize-recording", [temp, path, id, expected_duration,
                                        expected_meta_duration]() mutable
                {
        const double got = probe_audio_file_duration(temp);
        // `expected_duration` comes from the stream mpv just played, so it
        // cannot detect the backend handing us an entirely different song
        // with its own self-consistent length. Compare with the track's
        // metadata too: a large disagreement means wrong audio, not a
        // truncated download.
        const double meta = expected_meta_duration > 0 ? (double)expected_meta_duration : 0.0;
        const double tolerance = std::max(10.0, meta * 0.2);
        const bool wrong_song = meta > 0.0 && std::abs(got - meta) > tolerance;
        if (got <= 0.0 || (expected_duration > 0.0 && got < expected_duration - 5.0) ||
            wrong_song)
        {
            fprintf(stderr,
                    "Discarding recording %s: duration %.1fs (stream %.1fs, metadata %ds)%s\n",
                    id.c_str(), got, expected_duration, expected_meta_duration,
                    wrong_song ? " [wrong song]" : "");
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
    g_recording_expected_duration = 0;
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
    // Never let a stale cover from another row reach the play log: the
    // repair worker trusts it and would copy it into cached metadata.
    ev.thumbnail_url = authoritative_thumbnail_url(
        g_state.current_track_id, g_state.current_thumbnail_url);
    ev.ts = (int64_t)time(nullptr);
    g_state.play_log.push_back(ev);
    if ((int)g_state.play_log.size() > kPlayLogCap)
        g_state.play_log.erase(g_state.play_log.begin(),
                               g_state.play_log.end() - kPlayLogCap);
    g_state.stats_dirty = true;
    g_counts_dirty = true;
}


// ----------------------------------------------------------------------
// Video surface
//
// Audio always plays on its own player. The video page uses the idle mpv
// instance to decode a MUTED video stream (aid=no), kept on the audible
// player's timeline. Players never swap, so opening/closing the video or a
// buffer hiccup on the video side can never interrupt the sound. While the
// page is open, the muted video mirrors pause and follows seeks/drift.
// A stream without video (or a failed resolve) leaves playback untouched and
// only raises a toast.
// ----------------------------------------------------------------------
bool g_video_target_failed = false;
bool g_video_follow_track = false;
std::chrono::steady_clock::time_point g_video_requested_at{};

// Player decoding the muted video while the page is open or preloading.
static MpvPlayer *g_video_target = nullptr;
static bool g_video_seek_done = false;
// True once the preloading player has fully opened its new file (FILE_LOADED,
// not START_FILE): only then is the track list meaningful.
static bool g_video_file_loaded = false;
// Throttle for alignment/seeking of the muted video.
static std::chrono::steady_clock::time_point g_video_last_seek{};
// Last audible-player seek generation the video has followed, and when it
// was noticed (the audio's reported position lands a few frames later).
static std::uint64_t g_video_audio_seek_gen = 0;
static std::chrono::steady_clock::time_point g_video_audio_seek_at{};
// Speed currently applied to the muted video player. Small nudges around the
// user's speed keep it locked to the audio clock without seeking (which
// would starve the video cache).
static double g_video_applied_speed = 1.0;
// Constant offset between the song timeline and the matched video's timeline
// (video_time = song_time + offset). Estimated once per pair by the backend
// (audio cross-correlation), cached there, 0 for the track's own upload.
static double g_video_sync_offset = 0.0;
// When the preload first became fully ready (seek done, playing, aligned).
// The reveal waits a beat so the texture has live frames, not the pre-seek
// still that used to be visible at the start of the animation.
static std::chrono::steady_clock::time_point g_video_ready_at{};
// No speed nudges / corrective seeks while the reveal animation runs: an mpv
// speed change mid-animation can stall the decoder for a beat.
static std::chrono::steady_clock::time_point g_video_hold_sync_until{};
// Closing the page keeps the muted stream loaded and paused: reopening the
// same track is then instant (no resolve, no reload; just a seek to catch up).
static bool g_video_retained = false;
static std::string g_video_retained_id;
// Backend resolve results cross from the worker to the main loop here.
struct VideoResolveResult
{
    std::string id;
    std::string stream_id; // id whose stream was resolved (own or matched)
    std::string video;
    std::string audio;
    std::string error;
    std::string title;   // matched music video's title, or the track title
    std::string artist;
    bool matched = false;
    bool self_video = false;
    // Interim result (the track's own upload, shown while the matched music
    // video resolves) and a later hot-swap onto the matched video.
    bool interim = false;
    bool replace = false;
    bool retry_failed = false;
    double sync_offset = 0.0;
    bool sync_confident = false;
    std::uint64_t gen = 0;
};
static std::mutex g_video_result_mutex;
static std::shared_ptr<const VideoResolveResult> g_video_result;
static std::uint64_t g_video_request_gen = 0;
// Fresh-resolve retry state for a stream that fails to load (typically an
// expired/revoked googlevideo URL).
static std::string g_video_stream_id;
static int g_video_height = 720;
static bool g_video_fresh_tried = false;
static std::shared_ptr<const VideoResolveResult> g_video_last_result;
// When the current file started loading; a stream that never reaches
// FILE_LOADED (silent 403/stall) is treated as a failure after a few seconds.
static std::chrono::steady_clock::time_point g_video_load_at{};
// The background fresh-resolve is bounded: the full client walk can otherwise
// take minutes, leaving the page loading forever.
static std::chrono::steady_clock::time_point g_video_retry_deadline{};
// A/V offset computed after the video has already started.
struct VideoSyncMsg
{
    std::string song_id;
    double offset = 0.0;
    bool confident = false;
    std::uint64_t gen = 0;
};
static std::shared_ptr<const VideoSyncMsg> g_video_sync_msg;

// Events from the video player. START_FILE is only a hint: mpv's ytdl hook
// emits it before the resolved file is open, so readiness is FILE_LOADED.
static void video_target_event(const std::string &event)
{
    DLOG("video: event '%s'", event.c_str());
    if (event == "loaded")
    {
        g_video_file_loaded = true;
        g_video_load_at = {};
        g_video_retry_deadline = {};
    }
    else if (event.rfind("error", 0) == 0 || event == "end" || event == "stop")
        g_video_target_failed = true;
}

// Stop the muted video player without touching the audible one.
// `keep_frame` leaves the render surface attached so the page (if visible)
// keeps showing the last frame while the next video preloads.
static void stop_video_target(bool keep_frame)
{
    if (g_video_target)
    {
        if (g_video_target == display_player())
            return; // safety: never stop the player that is making sound
        g_video_target->set_event_callback(g_video_target == g_player.get() ? g_main_event_cb
                                                                           : g_fade_event_cb);
        g_video_target->set_volume(0);
        g_video_target->set_stream_record("");
        g_video_target->stop();
        g_video_target->set_video_enabled(false);
        g_video_target->set_audio_enabled(true);
        // Drop any sync nudge so the idle player carries the user's speed into
        // its next role (crossfade helper).
        const double base = std::max(0.25, (double)g_state.settings.playback_speed);
        g_video_target->set_speed(base);
        g_video_applied_speed = base;
        g_video_target = nullptr;
    }
    g_video_retained = false;
    g_video_retained_id.clear();
    g_video_sync_offset = 0.0;
    if (!keep_frame)
        video_surface().detach();
}

void begin_video_playback()
{
    if (!g_player || !g_fade_player || g_state.current_track_id.empty())
        return;
    if (g_state.video_loading.load())
        return; // a preload is already in flight

    // Instant reopen: the same track's muted stream is still loaded, paused.
    if (g_video_retained && g_video_retained_id == g_state.current_track_id &&
        g_video_target)
    {
        g_video_retained = false;
        g_state.video_tab = g_state.current_tab;
        g_state.video_loading = false;
        g_state.video_active = true;
        g_state.video_note.clear();
        MpvPlayer *audible = display_player();
        if (audible && g_video_target->cached_time_pos() >= 0.0)
            g_video_target->seek(std::max(0.0, audible->cached_time_pos() + g_video_sync_offset));
        g_video_audio_seek_gen = audible ? audible->seek_generation() : 0;
        g_video_audio_seek_at = {};
        const double base = std::max(0.25, (double)g_state.settings.playback_speed);
        g_video_target->set_speed(base);
        g_video_applied_speed = base;
        g_video_hold_sync_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(tunable_int("video.sync_hold_ms"));
        g_video_target->set_paused(!g_state.playing.load());
        g_video_target->set_volume(0);
        g_state.repaint_requested = true;
        DLOG("video: reusing the retained stream");
        return;
    }
    g_video_retained = false;
    g_video_retained_id.clear();
    g_video_sync_offset = 0.0;
    g_video_ready_at = {};
    g_video_fresh_tried = false;
    g_video_stream_id.clear();
    g_video_last_result.reset();

    // Free the idle player: an in-flight audio crossfade uses the helper.
    settle_inflight_crossfade();
    // A follow-up video reuses the same idle player; keep the page's frame.
    stop_video_target(g_state.video_active);
    g_state.video_tab = g_state.current_tab;

    MpvPlayer *audible = display_player();
    MpvPlayer *target = (audible == g_player.get()) ? g_fade_player.get() : g_player.get();
    if (!target || target == audible)
        return;
    if (!video_surface().attached_to(*target) && !video_surface().attach(*target))
    {
        ui_notify(UiNotifyType::Info, "Video unavailable",
                  "The video output could not be created on this display.", 4.0f);
        return;
    }

    // Use the same (fixed) render size the page will use, so showing it
    // later does not re-allocate the texture mid-animation.
    {
        int tw = 0, th = 0;
        video_render_target_size(video_surface().aspect(), tw, th);
        video_surface().set_target_size(tw, th);
    }
    target->set_event_callback(video_target_event);
    target->set_stream_record("");
    target->set_volume(0);
    target->set_audio_enabled(false); // muted: sound stays on the audio player
    target->set_video_enabled(true);

    g_video_target = target;
    g_video_target_failed = false;
    g_video_seek_done = false;
    g_video_file_loaded = false;
    const std::uint64_t gen = ++g_video_request_gen; // invalidate older resolves
    g_video_requested_at = std::chrono::steady_clock::now();
    g_state.video_loading = true;
    g_state.video_note.clear();
    g_state.repaint_requested = true;

    const std::string id = g_state.current_track_id;
    if (id.rfind("local:", 0) == 0)
    {
        DLOG("video: preloading local file %s", g_state.current_track_url.c_str());
        target->play_video(g_state.current_track_url);
        target->set_paused(!g_state.playing.load());
        return;
    }

    // Offline copy: play it from disk (instant, no resolve, works offline).
    const std::string cached = find_cached_video_path(id);
    if (!cached.empty())
    {
        DLOG("video: preloading cached file %s", cached.c_str());
        g_state.current_video_title = g_state.current_title;
        g_state.video_note.clear();
        target->play_video(cached);
        target->set_paused(!g_state.playing.load());
        return;
    }

    // Quality cap for the resolve (0 = 720p, 1 = 1080p, 2 = window height).
    int height = 720;
    if (g_state.settings.video_quality == 1)
        height = 1080;
    else if (g_state.settings.video_quality == 2)
        height = (int)ImGui::GetIO().DisplaySize.y;
    height = std::clamp(height, 360, 1080);
    g_video_height = height;
    const std::string track_title = g_state.current_title;
    const std::string track_artist = g_state.current_artist;
    MpvPlayer *audible_now = display_player();
    const int song_duration = audible_now ? (int)std::lround(std::max(0.0, audible_now->cached_duration())) : 0;
    // One-time analysis; explicitly skipped in low power mode.
    const bool allow_sync = !power::low_power();

    DLOG("video: resolving %s via the backend (h=%d)", id.c_str(), height);
    tasks::spawn("video-resolve", [id, gen, height, track_title, track_artist,
                                   song_duration, allow_sync]()
    {
        // The official-video lookup runs on the background channel while the
        // fast resolve of the track's own upload happens in parallel.
        MusicVideoMatch match;
        std::thread match_thread([&]() { match = fetch_music_video_prefetch(id); });
        VideoStreamUrls own = fetch_video_stream_urls(id, false, height, true);
        match_thread.join();

        const bool matched = match.ok && match.matched && !match.id.empty();
        const bool self_video = matched && match.self_video;
        const std::string stream_id = matched ? match.id : id;
        const bool upgrade = matched && stream_id != id;

        auto make_result = [&](const VideoStreamUrls &urls, bool is_matched,
                               const std::string &title, const std::string &artist,
                               bool is_interim, bool is_replace) {
            auto res = std::make_shared<VideoResolveResult>();
            res->id = id;
            res->stream_id = stream_id;
            res->gen = gen;
            res->video = urls.video;
            res->audio = urls.audio;
            res->error = urls.error;
            res->matched = is_matched;
            res->self_video = is_matched && self_video;
            res->title = title;
            res->artist = artist;
            res->interim = is_interim;
            res->replace = is_replace;
            return res;
        };
        auto post = [&](const std::shared_ptr<VideoResolveResult> &res) {
            std::lock_guard<std::mutex> lock(g_video_result_mutex);
            if (!g_video_result || g_video_result->gen <= gen)
                g_video_result = res;
            g_state.repaint_requested = true;
        };

        // Instant start: the song's own upload is always playable and can be
        // shown while the matched music video resolves. It is replaced in
        // place once the match is ready (no page close, nothing to re-open).
        if (upgrade && !own.video.empty())
            post(make_result(own, false, track_title, track_artist, true, false));

        VideoStreamUrls urls = upgrade ? VideoStreamUrls{} : own;
        if (upgrade)
        {
            urls = fetch_video_stream_urls(stream_id, false, height, true);
            if (urls.video.empty())
                urls = fetch_video_stream_urls(stream_id, false, height, false);
        }
        else if (urls.video.empty())
        {
            // Fast path failed: full fallback walk in the background.
            urls = fetch_video_stream_urls(stream_id, false, height, false);
        }

        double sync_offset = 0.0;
        bool sync_confident = false;
        bool sync_pending = false;
        if (matched && !self_video && !urls.video.empty())
        {
            VideoSyncResult cached = fetch_video_sync_cached(id, stream_id);
            if (cached.ok && cached.confident)
            {
                sync_offset = cached.offset;
                sync_confident = true;
            }
            else
            {
                sync_pending = allow_sync && (cached.ok || !cached.error.empty());
            }
        }

        auto final_res = make_result(urls, matched,
                                     matched ? match.title : track_title,
                                     matched ? match.artist : track_artist,
                                     false, upgrade);
        final_res->sync_offset = sync_offset;
        final_res->sync_confident = sync_confident;
        post(final_res);

        if (sync_pending)
        {
            VideoSyncResult sync = fetch_video_sync(id, stream_id, song_duration);
            auto msg = std::make_shared<VideoSyncMsg>();
            msg->song_id = id;
            msg->gen = gen;
            msg->offset = sync.ok ? sync.offset : 0.0;
            msg->confident = sync.ok && sync.confident;
            {
                std::lock_guard<std::mutex> lock(g_video_result_mutex);
                g_video_sync_msg = std::move(msg);
            }
            g_state.repaint_requested = true;
        }
    });
}

// Main-loop side of the backend resolve: load the video URL into the muted
// player. Only the video URL is used; audio keeps coming from its own player.
void video_preload_poll()
{
    if (!g_video_target)
        return;
    // Results arrive while loading (first load) or while the page is open
    // (hot-swap from the track's own upload to the matched music video).
    if (!g_state.video_loading.load() && !g_state.video_active)
        return;
    std::shared_ptr<const VideoResolveResult> result;
    {
        std::lock_guard<std::mutex> lock(g_video_result_mutex);
        result = std::move(g_video_result);
        g_video_result.reset();
    }
    if (!result)
        return;
    if (result->gen != g_video_request_gen || result->id != g_state.current_track_id)
    {
        DLOG("video: dropping stale resolve %s", result->id.c_str());
        return;
    }
    if (result->video.empty())
    {
        if (result->retry_failed && g_state.video_active)
        {
            ui_notify(UiNotifyType::Warning, "Video unavailable",
                      "Playing the audio only.", 4.0f);
            end_video_playback();
        }
        else if (!g_state.video_active)
        {
            cancel_video_resolve(true);
        }
        return;
    }

    g_video_stream_id = result->stream_id;
    g_video_last_result = result;
    const bool hot = result->replace && g_state.video_active;
    DLOG("video: resolved (%s, sync %s %.2fs%s)",
         result->self_video ? "already a video"
                            : (result->matched ? "matched music video" : "track upload"),
         result->sync_confident ? "offset" : "none", result->sync_offset,
         hot ? ", hot-swap" : "");
    g_video_sync_offset = result->sync_offset;
    g_state.current_video_title = result->title.empty() ? g_state.current_title : result->title;
    if (result->interim)
        g_state.video_note.clear();
    else
        g_state.video_note = result->matched
                                 ? ""
                                 : "No music video match - showing this track's own video.";

    g_video_target->play_video(result->video);
    g_video_load_at = std::chrono::steady_clock::now();
    g_video_target->set_paused(!g_state.playing.load());
    if (hot)
    {
        // Keep the page up; jump the new stream to the audio position.
        MpvPlayer *audible = display_player();
        if (audible)
            g_video_target->seek(std::max(
                0.0, audible->cached_time_pos() + g_video_sync_offset + 0.1));
        g_state.video_loading = false;
        g_state.repaint_requested = true;
    }
}


// One retry after a stream-load failure, bypassing all URL caches. URLs are
// short-lived; a URL that resolved last night can 403 in the morning.
static bool retry_video_stream()
{
    if (g_video_fresh_tried || g_video_stream_id.empty() || !g_video_last_result)
        return false;
    g_video_fresh_tried = true;
    g_video_target_failed = false;
    g_video_retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(tunable_int("video.retry_deadline_sec"));
    auto base = g_video_last_result;
    const std::string sid = g_video_stream_id;
    const int h = g_video_height;
    const std::uint64_t gen = g_video_request_gen;
    DLOG("video: fresh re-resolve after a load failure (%s)", sid.c_str());
    tasks::spawn("video-retry", [base, sid, h, gen]()
    {
        VideoStreamUrls urls = fetch_video_stream_urls(sid, true, h, false);
        auto res = std::make_shared<VideoResolveResult>(*base);
        res->gen = gen;
        res->video = std::move(urls.video);
        res->audio = std::move(urls.audio);
        res->error = std::move(urls.error);
        res->replace = true;
        res->retry_failed = res->video.empty();
        {
            std::lock_guard<std::mutex> lock(g_video_result_mutex);
            if (!g_video_result || g_video_result->gen <= gen)
                g_video_result = std::move(res);
        }
        g_state.repaint_requested = true;
    });
    return true;
}

// Called once per frame while a preload is in flight, after the surface has
// tried to render a frame. Shows the page once a real, aligned frame exists.
void video_preload_tick(bool have_frame)
{
    if (!g_state.video_loading.load() || !g_video_target)
        return;
    if (g_video_target_failed)
    {
        if (!retry_video_stream())
            cancel_video_resolve(true);
        return;
    }
    g_video_target->set_paused(!g_state.playing.load());
    if (!g_video_file_loaded)
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_video_retry_deadline != std::chrono::steady_clock::time_point{} &&
            now > g_video_retry_deadline)
        {
            g_video_retry_deadline = {};
            DLOG("video: retry timed out");
            cancel_video_resolve(true);
            return;
        }
        // A stream that never reports FILE_LOADED (mpv opened it, then the
        // server refused the bytes) gets one fresh re-resolve; the request
        // timeout below still covers a resolve that never returned.
        if (g_video_load_at != std::chrono::steady_clock::time_point{} &&
            now - g_video_load_at > std::chrono::seconds(tunable_int("video.load_fail_sec")))
        {
            if (!retry_video_stream())
                cancel_video_resolve(true);
            return;
        }
        if (now - g_video_requested_at > std::chrono::seconds(tunable_int("video.resolve_timeout_sec")))
            cancel_video_resolve(true); // resolve never finished
        return;
    }
    if (!g_video_target->has_video())
    {
        cancel_video_resolve(true); // audio-only upload, no video track
        return;
    }
    if (!g_video_seek_done)
    {
        MpvPlayer *audible = display_player();
        const double pos = audible ? audible->cached_time_pos() : -1.0;
        if (pos > 0.5)
            g_video_target->seek(std::max(0.0, pos + g_video_sync_offset));
        g_video_seek_done = true;
        g_video_last_seek = std::chrono::steady_clock::now();
        return;
    }
    // Show as soon as the stream has a video frame. The previous strict gate
    // (seek/idle/alignment/readiness hold) could block the reveal forever on
    // a stream that had loaded fine. One alignment seek is issued here; the
    // active tick keeps drift and seeks locked once the page is up.
    if (!g_video_target->has_video())
    {
        if (!retry_video_stream())
            cancel_video_resolve(true);
        return;
    }
    if (!have_frame)
        return;
    if (!g_video_seek_done)
    {
        MpvPlayer *audible = display_player();
        const double pos = audible ? audible->cached_time_pos() : -1.0;
        if (pos > 0.5)
            g_video_target->seek(std::max(0.0, pos + g_video_sync_offset));
        g_video_seek_done = true;
        g_video_last_seek = std::chrono::steady_clock::now();
        return;
    }
    finish_video_start();
}

// Main-loop side of the deferred A/V analysis. Applied as a one-time seek
// (or stored for the retained reopen) when the background result arrives.
void video_sync_poll()
{
    std::shared_ptr<const VideoSyncMsg> msg;
    {
        std::lock_guard<std::mutex> lock(g_video_result_mutex);
        msg = std::move(g_video_sync_msg);
        g_video_sync_msg.reset();
    }
    if (!msg)
        return;
    if (!msg->confident || msg->gen != g_video_request_gen ||
        msg->song_id != g_state.current_track_id || !g_video_target)
    {
        DLOG("video: dropping sync result for %s", msg->song_id.c_str());
        return;
    }
    g_video_sync_offset = msg->offset;
    DLOG("video: sync offset applied %.2fs", msg->offset);
    if (g_state.video_active)
    {
        MpvPlayer *audible = display_player();
        if (audible)
            g_video_target->seek(std::max(
                0.0, audible->cached_time_pos() + g_video_sync_offset + 0.1));
    }
    g_state.repaint_requested = true;
}

void finish_video_start()
{
    if (!g_video_target)
    {
        cancel_video_resolve(false);
        return;
    }
    g_state.video_loading = false;
    g_state.video_active = true;
    g_video_target_failed = false;
    MpvPlayer *audible = display_player();
    g_video_audio_seek_gen = audible ? audible->seek_generation() : 0;
    g_video_audio_seek_at = {};
    g_video_ready_at = {};
    const double base = std::max(0.25, (double)g_state.settings.playback_speed);
    g_video_target->set_speed(base);
    g_video_applied_speed = base;
    g_video_target->set_paused(!g_state.playing.load());
    g_video_target->set_volume(0);
    g_video_hold_sync_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(tunable_int("video.sync_hold_ms"));
    g_state.repaint_requested = true;
    DLOG("video: showing (audio=%.2f video=%.2f)",
         audible ? audible->cached_time_pos() : -1.0,
         g_video_target->cached_time_pos());
}

// While the page is open: mirror pause and follow user seeks. The two
// decoders run on independent clocks; they stay within a frame of each other
// on their own, so only an explicit seek (or a large stall) is corrected --
// seeking every frame would starve the video cache and freeze the picture.
void video_active_tick()
{
    if (!g_state.video_active || !g_video_target)
        return;
    if (g_video_target_failed)
    {
        if (retry_video_stream())
        {
            DLOG("video: retrying the stream after a failure");
            return;
        }
        DLOG("video: stream ended while active");
        g_video_target_failed = false;
        end_video_playback();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // Do no mpv work at all while the reveal runs: even synchronous property
    // reads queue behind the player's decoder and were stalling the UI thread.
    if (now < g_video_hold_sync_until)
        return;

    const bool want_pause = !g_state.playing.load();
    if (g_video_target->is_paused() != want_pause)
        g_video_target->set_paused(want_pause);
    if (g_video_target->is_buffering())
        return;

    MpvPlayer *audible = display_player();
    if (!audible)
        return;

    // A user seek bumps the audible player's seek generation. Give mpv a
    // moment to report the landed position, then move the video there too.
    const std::uint64_t gen = audible->seek_generation();
    if (gen != g_video_audio_seek_gen)
    {
        g_video_audio_seek_gen = gen;
        g_video_audio_seek_at = now;
        return;
    }
    if (g_video_audio_seek_at != std::chrono::steady_clock::time_point{})
    {
        if (now - g_video_audio_seek_at < std::chrono::milliseconds(tunable_int("video.seek_follow_ms")))
            return;
        g_video_audio_seek_at = {};
        g_video_target->seek(std::max(0.0, audible->cached_time_pos() + g_video_sync_offset + 0.1));
        g_video_last_seek = now;
        return;
    }

    // Warm the next track's match decision while the page is open: the
    // search is the slow part of opening, and the backend caches it on disk,
    // so advancing (or pressing V on the next track) starts instantly.
    {
        static std::string s_prefetched_next;
        std::string next_id;
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            if (!g_state.play_queue.empty())
                next_id = g_state.play_queue.front().id;
        }
        if (!next_id.empty() && next_id != g_state.current_track_id &&
            next_id != s_prefetched_next)
        {
            s_prefetched_next = next_id;
            tasks::spawn("video-match-prefetch", [next_id]()
            {
                fetch_music_video_prefetch(next_id);
            });
        }
    }

    // Recover only from a large drift (e.g. the video stream stalled and the
    // audio kept going), and do it rarely.
    const double audio_pos = audible->cached_time_pos();
    const double tpos = g_video_target->cached_time_pos();
    const double want_pos = audio_pos + g_video_sync_offset;
    if (audio_pos >= 0.0 && tpos >= 0.0 && std::fabs(tpos - want_pos) > tunable("video.drift_threshold_sec") &&
        now - g_video_last_seek > std::chrono::seconds(tunable_int("video.drift_seek_interval_sec")))
    {
        DLOG("video: resync (audio=%.2f video=%.2f offset=%.2f)", audio_pos, tpos,
             g_video_sync_offset);
        g_video_target->seek(std::max(0.0, want_pos + 0.1));
        g_video_last_seek = now;
        return;
    }

    // Fine lock: nudge the muted video's speed toward the audio position.
    // Seeking for sub-second differences would drop the buffered video and
    // cause the stutter; a few percent of speed is imperceptible and closes
    // the gap within a couple of seconds.
    const double base = std::max(0.25, (double)g_state.settings.playback_speed);
    double desired = base;
    if (audio_pos >= 0.0 && tpos >= 0.0 && std::fabs(tpos - want_pos) > tunable("video.speed_deadband"))
        desired = base + std::clamp((want_pos - tpos) * (double)tunable("video.speed_nudge"),
                                                   -(double)tunable("video.speed_clamp"),
                                                   (double)tunable("video.speed_clamp"));
    if (std::fabs(desired - g_video_applied_speed) > tunable("video.speed_apply_threshold"))
    {
        g_video_target->set_speed(desired);
        g_video_applied_speed = desired;
    }
}

void cancel_video_resolve(bool notify)
{
    stop_video_target(false);
    g_state.video_loading = false;
    g_state.video_theater = false;
    g_video_target_failed = false;
    g_video_seek_done = false;
    g_video_file_loaded = false;
    g_video_ready_at = {};
    g_state.repaint_requested = true;
    // Invalidate a resolve that is still running on its worker thread.
    ++g_video_request_gen;
    {
        std::lock_guard<std::mutex> lock(g_video_result_mutex);
        g_video_result.reset();
    }
    if (notify)
        ui_notify(UiNotifyType::Info, "No video for this track",
                  "Playing the audio only.", 3.5f);
    DLOG("video: no video here, staying on audio");
}

void end_video_playback()
{
    if (!g_state.video_active && !g_state.video_loading.load())
        return;
    if (g_state.video_loading.load() && !g_state.video_active)
    {
        cancel_video_resolve(false);
        return;
    }
    g_state.video_active = false;
    g_state.video_loading = false;
    g_state.video_theater = false;
    g_video_follow_track = false;
    // Keep the muted stream loaded and paused: the page fades out on the
    // retained frame and reopening this track is instant. The audible player
    // is untouched. Track changes release it (see begin_play_track).
    if (g_video_target)
    {
        g_video_target->set_paused(true);
        g_video_target->set_volume(0);
        g_video_retained = true;
        g_video_retained_id = g_state.current_track_id;
    }
    g_state.repaint_requested = true;
    DLOG("video: back to audio (retained)");
}

void begin_play_track(const Track &t, int index)
{
    if (t.id.empty())
        return;
    g_endless_waiting = false;
    stop_fade();
    // Track change invalidates the retained/preloaded video. If the page is
    // open, keep its last frame until the new video is ready.
    stop_video_target(g_state.video_active);
    // Invalidate resolves still in flight for the previous selection; this
    // request's generation is captured when the resolver thread is spawned.
    g_play_generation.fetch_add(1);
    if (!g_force_fresh_resolve)
        g_play_retry_count = 0;
    g_state.loading = true;
    g_loading_since = std::chrono::steady_clock::now();
    g_state.current_title = t.title;
    g_state.current_artist = t.artist;
    const std::string thumb_url = playback_thumb_url(t);
    g_state.current_thumbnail_url = thumb_url;
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
    g_state.pending_play_url = playable_track_url(t.id, t.url);
    g_state.pending_play_title = t.title;
    g_state.pending_play_artist = t.artist;
    g_state.pending_play_thumbnail_url = thumb_url;
    g_state.pending_play_duration_sec = t.duration_sec;
    g_state.pending_video_id = t.id;
    g_state.pending_video_url = playable_track_url(t.id, t.url);
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

static Track current_track_snapshot()
{
    Track t;
    t.id = g_state.current_track_id;
    t.title = g_state.current_title;
    t.artist = g_state.current_artist;
    t.artist_id = g_state.current_artist_id;
    t.album_id = g_state.current_album_id;
    t.thumbnail_url = g_state.current_thumbnail_url;
    t.url = g_state.current_track_url;
    if (t.id == g_state.pending_video_id)
        t.duration_sec = g_state.pending_play_duration_sec;
    return t;
}

void note_playback_navigation()
{
    static std::string s_last_id;
    const std::string &id = g_state.current_track_id;
    if (id.empty())
    {
        s_last_id.clear();
        return;
    }
    if (id == s_last_id)
    {
        // A navigation press that landed on the current track has nothing to
        // consume (and must not leave a stale pending id behind).
        if (g_state.nav_pending_id == id)
            g_state.nav_pending_id.clear();
        return;
    }
    s_last_id = id;

    if (g_state.nav_pending_id == id)
    {
        // Previous/Next already moved the cursor and inserted the entry.
        g_state.nav_pending_id.clear();
        return;
    }
    // Any other track change is a new play: it discards the forward branch.
    if (!g_state.nav_pending_id.empty())
        g_state.nav_pending_id.clear();

    if (g_state.nav_pos >= 0 &&
        g_state.nav_pos + 1 < (int)g_state.nav_history.size())
        g_state.nav_history.resize((std::size_t)g_state.nav_pos + 1);
    if (g_state.nav_pos >= 0 && g_state.nav_pos < (int)g_state.nav_history.size() &&
        g_state.nav_history[(std::size_t)g_state.nav_pos].id == id)
        return; // replay/restart of the entry already under the cursor

    g_state.nav_history.push_back(current_track_snapshot());
    if (g_state.nav_history.size() > 200)
        g_state.nav_history.erase(g_state.nav_history.begin());
    g_state.nav_pos = (int)g_state.nav_history.size() - 1;
}

static bool start_nav_track(const Track &t)
{
    if (t.id.empty())
        return false;
    g_state.nav_pending_id = t.id;
    if (!try_start_manual_crossfade(t, -1))
        begin_play_track(t, -1);
    g_state.repaint_requested = true;
    return true;
}

void play_previous_track()
{
    if (g_state.current_track_id.empty())
        return;
    if (g_state.nav_pos > 0 &&
        g_state.nav_pos <= (int)g_state.nav_history.size())
    {
        const Track target = g_state.nav_history[(std::size_t)g_state.nav_pos - 1];
        --g_state.nav_pos;
        start_nav_track(target);
        return;
    }

    // Start of the in-memory history: step to the previous row of the active
    // list (when that list still contains the current track).
    auto playlist = get_playlist(g_state);
    const int idx = g_state.current_track_index;
    if (playlist && idx > 0 && idx < (int)playlist->size() &&
        (*playlist)[(std::size_t)idx].id == g_state.current_track_id)
    {
        const Track target = (*playlist)[(std::size_t)idx - 1];
        if (g_state.nav_history.empty())
            g_state.nav_history.push_back(current_track_snapshot());
        const int at = std::max(0, g_state.nav_pos);
        // Insert ahead of the current entry: the cursor lands on the list row
        // and the track we came from remains as the forward entry.
        g_state.nav_history.insert(g_state.nav_history.begin() + at, target);
        g_state.nav_pos = at;
        start_nav_track(target);
        return;
    }

    replay_current_track();
}

void play_next_track()
{
    if (g_state.nav_pos >= 0 &&
        g_state.nav_pos + 1 < (int)g_state.nav_history.size())
    {
        const Track target = g_state.nav_history[(std::size_t)g_state.nav_pos + 1];
        ++g_state.nav_pos;
        start_nav_track(target);
        return;
    }
    advance_playback(true);
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
    static std::chrono::steady_clock::time_point s_retry_at{};
    static int s_retry_backoff = 0;
    const auto now = std::chrono::steady_clock::now();

    // A refill that came back empty (network hiccup, rate limit) must not
    // strand the queue at EOF. While the player waits for tracks, retry with
    // a bounded backoff instead of giving up after the first attempt.
    if (g_endless_waiting && !g_state.endless_loading.load() &&
        s_retry_at != std::chrono::steady_clock::time_point{} && now >= s_retry_at)
    {
        s_retry_at = {};
        g_endless_attempted = false;
        if (!maybe_start_endless_refill())
        {
            g_endless_waiting = false;
            g_state.status_text = "Ready.";
        }
    }

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

    if (g_endless_waiting && !appended && !g_state.endless_loading.load())
    {
        // Keep waiting: schedule the next attempt. 4 s, then 8/16/32 s.
        s_retry_backoff = std::min(s_retry_backoff + 1, tunable_int("playback.retry_max_doublings"));
        s_retry_at = now + std::chrono::seconds(2 << s_retry_backoff);
        g_state.status_text = "Endless radio unavailable; retrying...";
    }
    else if (g_endless_waiting)
    {
        g_endless_waiting = false;
        s_retry_at = {};
        s_retry_backoff = 0;
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
    g_state.current_thumbnail_url = playback_thumb_url(consumed);
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
        history_entry.thumbnail_url = playback_thumb_url(consumed);
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
    if (g_state.video_active || g_state.video_loading.load())
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
    // Keep the user's play/pause intent. A pause during the fade must survive
    // the promotion: the ramp mirrors it onto both players, so forcing
    // playing=true here would leave the UI claiming playback while mpv sits
    // paused. The promoted player already carries the pause flag; restate it
    // from the app's intent in case it started paused.
    g_player->set_paused(!g_state.playing.load());
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
    g_state.status_text = g_state.playing.load() ? "Playing" : "Paused";

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
    // The helper player must be free for the fade.
    stop_video_target(false);
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

    // Video plays with a hard cut: never start or continue an audio fade
    // while the video surface is up or a video is being preloaded (the idle
    // player is busy with that preload).
    if (g_state.video_active || g_state.video_loading.load())
        return;

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
            // Nothing is pending or active; clear the target too so a later
            // fade cannot reuse a stale track/index.
            g_cf.manual = false;
            g_cf.track = Track{};
            g_cf.index = -1;
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
        if (ramp_now - s_last_ramp < std::chrono::milliseconds(tunable_int("playback.resume_ramp_ms")))
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

    // Seeking is asynchronous: cached_time_pos can still report the pre-seek
    // position for a frame or two (more visibly on Windows, where mpv's
    // property observer and the render loop race). Starting a natural
    // crossfade from that stale position made a backward seek near the end
    // jump to the next song. The hold also avoids fading away a track the
    // user deliberately seeked into the final seconds: let it play out.
    {
        static std::uint64_t last_seek_gen = 0;
        static std::chrono::steady_clock::time_point last_seek_at{};
        const std::uint64_t gen = g_player->seek_generation();
        const auto now = std::chrono::steady_clock::now();
        if (gen != last_seek_gen)
        {
            last_seek_gen = gen;
            last_seek_at = now;
        }
        const auto hold = std::chrono::milliseconds(
            (long long)(std::max(1.5, cf + 0.5) * 1000.0));
        if (now - last_seek_at < hold)
            return;
    }

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
    // The helper player must be free for this fade.
    stop_video_target(false);
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
    DLOG("EVENT %s", event.c_str());
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

        if (g_state.video_open_on_start)
        {
            g_state.video_open_on_start = false;
            begin_video_playback();
        }

        // Video page following the queue: the new track's audio is playing
        // now, so preload its video on the idle player and swap when a real
        // frame is ready (audio keeps running in the meantime).
        if (g_video_follow_track)
        {
            g_video_follow_track = false;
            DLOG("video: follow track start -> preload");
            begin_video_playback();
        }

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
            // Only apply the saved position to the track it belongs to. This
            // start event might be for a different song: the restored track
            // failed and playback auto-advanced, or the user picked another
            // one while it was still resolving.
            const bool restore_track =
                !g_state.session_restore.track_id.empty() &&
                g_state.current_track_id == g_state.session_restore.track_id;
            const double restore_pos =
                restore_track ? g_state.session_restore.position : 0.0;
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
        const std::string thumb_url = authoritative_thumbnail_url(
            g_state.current_track_id, g_state.current_thumbnail_url);
        std::string thumb_path = thumbnail_is_cached(thumb_url)
                                     ? get_cached_thumbnail_path(thumb_url)
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
            history_entry.thumbnail_url = authoritative_thumbnail_url(
                history_entry.id, g_state.current_thumbnail_url);
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

