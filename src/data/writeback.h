#pragma once
#include <functional>
#include <string>
#include <vector>

struct Track;

// Single background worker that serializes all YouTube Music account
// mutations (likes, playlist mirror edits) so they apply in issue order and
// never contend with each other on the backend pipe. Jobs are network-bound;
// callers return immediately.
void start_writeback_worker();
void stop_writeback_worker();
void writeback_enqueue(std::function<void()> job);

// Master switch (Settings checkbox, off by default). When disabled, every
// wb_* call is a no-op; re-enabling reconciles via the next playlist edit.
void writeback_set_enabled(bool enabled);
bool writeback_enabled();

// Like/unlike a track on the signed-in account (INDIFFERENT un-likes).
void wb_like(const std::string &video_id, bool liked);

// Playlist mirror. wb_sync_playlist reconciles the server playlist with the
// given local contents: on first call it creates the server playlist (the id
// is cached in <playlists>/<name>.meta.json); afterwards it removes
// server-side extras, appends missing tracks and reorders to match. Call it
// after every local playlist mutation (save_playlist sites).
void wb_sync_playlist(const std::string &name, const std::vector<Track> &tracks);
// Renames the server playlist (and the local meta file). Call after the
// local rename succeeded.
void wb_rename_playlist(const std::string &old_name, const std::string &new_name);
// Deletes the server playlist (if linked) and drops the local meta file.
void wb_delete_playlist(const std::string &name);
