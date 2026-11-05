#pragma once

#include "ui.h"

// Local SQLite library. This is the source of truth for playlists, likes,
// queue, history, play counts/log and saved albums/artists. Settings and the
// session snapshot intentionally stay as JSON.
namespace library
{
// Open/create the DB and import legacy JSON on first run. Returns false when
// the migration failed; in that case the old JSON persistence remains active.
bool init(std::string &error);

bool available();
std::string db_path();
std::string backup_dir();
std::string status_text();

// Track search (FTS5 title/artist/album).
std::vector<Track> search_tracks(const std::string &query, int limit = 200);

// Diagnostics/backup.
bool backup_to(const std::string &path, std::string &error);
bool integrity_check(std::string &result);

// Persistence (called by the legacy save_* wrappers in ui_helpers.cpp).
void save_liked(const std::vector<Track> &tracks);
std::vector<Track> load_liked();

void save_history(const std::vector<Track> &tracks);
std::vector<Track> load_history();

void save_queue(const std::vector<Track> &tracks);
std::vector<Track> load_queue();

void save_playlist(const std::string &name, const std::vector<Track> &tracks);
std::vector<Track> load_playlist_tracks(const std::string &name);
std::vector<std::string> playlist_names();
void delete_playlist(const std::string &name);

void save_play_counts(const std::unordered_map<std::string, int> &counts);
std::unordered_map<std::string, int> load_play_counts();

void save_play_log(const std::vector<PlayEvent> &events);
std::vector<PlayEvent> load_play_log();

void save_saved_albums(const std::vector<SavedAlbum> &albums);
std::vector<SavedAlbum> load_saved_albums();

void save_saved_artists(const std::vector<SavedArtist> &artists);
std::vector<SavedArtist> load_saved_artists();

// Saved podcast shows. Stored in the same SQLite library as the rest of the
// user's collection; the old saved_podcasts.json is only used for a one-time
// migration.
void save_saved_podcasts(const std::vector<PodcastShow> &shows);
std::vector<PodcastShow> load_saved_podcasts();

// Downloaded tracks stay on disk; this keeps their metadata in the library so
// Library search and diagnostics see the full collection even before the
// Downloads tab is opened.
void save_downloads(const std::vector<Track> &tracks);
long long download_count();

// Close the database and reset module state. The app calls this during
// shutdown so SQLite can checkpoint the WAL cleanly.
void shutdown();
void shutdown_for_tests();
} // namespace library
