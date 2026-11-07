#pragma once

// Shared internals for the app*.cpp translation units. Only what genuinely
// crosses a module boundary lives here; file-local helpers stay static.

#include <SDL2/SDL.h>
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "ui.h"
#include "crossfade.h"

#define DLOG(...) do { fputs("[p100] ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
#define DNOW() ((long long)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()))

class MpvPlayer;
class MprisPlayer;

extern AppState g_state;
extern std::unique_ptr<MpvPlayer> g_player;
extern MpvPlayer *g_player_raw;
extern std::unique_ptr<MpvPlayer> g_fade_player;
extern MprisPlayer *g_mpris;
extern std::function<void(const std::string &)> g_main_event_cb;
extern std::function<void(const std::string &)> g_fade_event_cb;
extern std::mutex g_data_mutex;

extern float g_ui_layout_scale;
extern std::unordered_map<std::string, float> g_ui_elem;

struct PlayRequest
{
    std::string path;      // local file or resolved stream URL
    std::string id;        // video id (for the offline cache)
    bool remote;           // true when path is a network stream to be recorded
    std::uint64_t gen = 0; // play generation the request was issued for
};
extern std::queue<PlayRequest> g_play_queue;
extern std::mutex g_play_queue_mutex;
extern std::atomic<std::uint64_t> g_play_generation;

extern std::map<std::string, std::string> g_prefetched_urls;
extern std::mutex g_prefetch_mutex;

extern bool g_endless_waiting;
extern std::string g_endless_seed;
extern bool g_endless_attempted;

extern bool g_force_fresh_resolve;
extern int g_play_retry_count;

extern crossfade::State g_cf;
extern std::chrono::steady_clock::time_point g_loading_since;
extern double g_pending_restore_seek;
extern std::string g_pending_restore_seek_id;
extern std::uint64_t g_pending_restore_seek_gen;
extern std::chrono::steady_clock::time_point g_pending_restore_seek_deadline;

inline constexpr int kMaxBgDownloads = 2;

extern std::atomic<bool> g_history_dirty;
extern std::atomic<bool> g_counts_dirty;

extern std::string g_recording_path;
extern std::string g_recording_temp;
extern std::string g_recording_id;
extern bool g_recording_started;
extern double g_recording_duration;
extern bool notify_init_done;

extern std::mutex g_play_fail_mutex;
extern std::string g_play_fail_hint;

extern std::atomic<bool> g_cover_sweep_running;
extern std::set<std::string> g_downloading;
extern std::mutex g_download_mutex;
extern std::atomic<int> g_bg_download_count;

struct NextPick
{
    Track track;
};

// ---- Playback (app_playback.cpp) ----
void handle_player_event(const std::string &event, MprisPlayer *mpris);
void stop_fade();
void resolve_audio(const std::string &video_id, const std::string &video_url,
                   const std::string &title, const std::string &artist,
                   const std::string &thumb, bool fresh = false,
                   std::uint64_t gen = 0);
std::string stream_container_ext(const std::string &url);
void rebuild_queue_from_source();
void prefetch_next_track();
void predownload_next_track();
void finalize_recording();
void discard_recording();
void commit_play_count();
void begin_play_track(const Track &t, int index);
void replay_current_track();
bool maybe_start_endless_refill();
void consume_endless_results();
void refill_queue_from_cycle();
bool select_next_track(bool allow_refill, bool consume, NextPick &out);
void adopt_fade_track(const Track &consumed, int consumed_index);
bool can_start_manual_crossfade(const Track &t);
void promote_fade_player_to_main();
void finish_manual_crossfade();
void settle_inflight_crossfade();
void start_manual_crossfade(const Track &t, int index, bool manual);
bool try_start_manual_crossfade(const Track &t, int index);
void maybe_start_crossfade();
bool advance_playback(bool manual = false);
MpvPlayer *display_player();
void update_discord_presence();

void show_notification(const std::string &title, const std::string &body,
                       const std::string &icon_path = "");

// ---- Background workers (app_workers.cpp) ----
void cover_sweep_worker(std::vector<Track> tracks);
void maybe_sweep_missing_covers();
void background_download(const std::string &video_id);
void load_recommendations(bool force = false);
void artist_search_and_update(const std::string &query, int expect_id);
bool text_matches_query(const std::string &haystack, const std::string &needle);
void merge_local_search_results(const std::string &query, SearchAllResult &results);
void search_all_and_update(const std::string &query, int expect_id);
void load_lyrics_worker(std::string track_id, int expect_seq);

// ---- Touch / wheel input (app_input.cpp) ----
void reset_wheel_scroll();
void process_touch_click();
void handle_touch_event(const SDL_Event &event, SDL_Window *window);
void update_touch_long_press();
bool update_touch_inertia();
bool update_wheel_inertia();
void handle_wheel_event(const SDL_Event &event);
void reset_pointer_gestures();
extern double g_touch_click_time;
