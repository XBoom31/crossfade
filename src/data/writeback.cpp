#include "writeback.h"

#include "ui.h"
#include "ui_notify.h"
#include "ytdlp.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace
{
std::mutex wb_mutex;
std::condition_variable wb_cv;
std::deque<std::function<void()>> wb_queue;
bool wb_started = false;
// Master switch: account mutations stay local unless the user opts in.
std::atomic<bool> g_wb_enabled{false};

void writeback_worker()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(wb_mutex);
            wb_cv.wait(lock, [] { return !wb_queue.empty(); });
            job = std::move(wb_queue.front());
            wb_queue.pop_front();
        }
        job(); // failures surface via take_backend_error() inside each job
    }
}

// <playlists>/<sanitized-name>.meta.json holds {"server_id": "..."} for the
// local→server playlist link. Kept out of the bare-array playlist JSON so
// load_playlist_tracks() is untouched.
std::string playlist_meta_path(const std::string &name)
{
    return playlists_dir() + "/" + sanitize_filename(name) + ".meta.json";
}

std::string extract_json_string(const std::string &data, const char *key)
{
    std::string needle = std::string("\"") + key + "\":";
    size_t k = data.find(needle);
    if (k == std::string::npos)
        return "";
    size_t q1 = data.find('"', k + needle.size());
    if (q1 == std::string::npos)
        return "";
    size_t q2 = data.find('"', q1 + 1);
    if (q2 == std::string::npos)
        return "";
    return data.substr(q1 + 1, q2 - q1 - 1);
}

std::string read_server_id(const std::string &meta_path)
{
    std::string data;
    if (!read_text_file(meta_path, data))
        return "";
    return extract_json_string(data, "server_id");
}

void write_server_id(const std::string &meta_path, const std::string &id)
{
    write_text_file(meta_path, "{\"server_id\":\"" + id + "\"}\n");
}

void notify_backend_failure()
{
    BackendError err = take_backend_error();
    if (err)
        ui_notify_backend_error(err);
}

} // namespace

void start_writeback_worker()
{
    std::lock_guard<std::mutex> lock(wb_mutex);
    if (!wb_started)
    {
        wb_started = true;
        std::thread(writeback_worker).detach();
    }
}

void writeback_set_enabled(bool enabled)
{
    g_wb_enabled.store(enabled);
}

bool writeback_enabled()
{
    return g_wb_enabled.load();
}

void writeback_enqueue(std::function<void()> job)
{
    if (!job)
        return;
    {
        std::lock_guard<std::mutex> lock(wb_mutex);
        wb_queue.push_back(std::move(job));
    }
    wb_cv.notify_one();
}

void wb_like(const std::string &video_id, bool liked)
{
    if (video_id.empty() || !g_wb_enabled.load())
        return;
    writeback_enqueue([video_id, liked] {
        if (!backend_rate_song(video_id, liked ? "LIKE" : "INDIFFERENT"))
            notify_backend_failure();
    });
}

void wb_sync_playlist(const std::string &name, const std::vector<Track> &tracks)
{
    if (name.empty() || !g_wb_enabled.load())
        return;
    writeback_enqueue([name, tracks] {
        // Unique local ids, in playlist order.
        std::vector<std::string> ids;
        std::set<std::string> idset;
        for (const Track &t : tracks)
        {
            if (!t.id.empty() && idset.insert(t.id).second)
                ids.push_back(t.id);
        }

        // 1. Ensure the server counterpart exists.
        std::string meta = playlist_meta_path(name);
        std::string server_id = read_server_id(meta);
        if (server_id.empty())
        {
            server_id = backend_create_playlist(name, ids);
            if (server_id.empty())
            {
                notify_backend_failure();
                return;
            }
            write_server_id(meta, server_id);
            return; // created seeded with `ids`; server order already matches
        }

        // 2. Fetch current server contents (videoId, setVideoId) in order.
        auto items = backend_playlist_items(server_id);
        if (items.empty() && !ids.empty())
        {
            BackendError err = take_backend_error();
            if (err)
            {
                ui_notify_backend_error(err);
                return;
            }
        }
        else
        {
            take_backend_error();
        }

        // 3. Remove server items that no longer exist locally.
        std::vector<std::pair<std::string, std::string>> removals;
        for (const auto &item : items)
        {
            if (!idset.count(item.first))
                removals.push_back(item);
        }
        if (!removals.empty())
        {
            if (!backend_remove_playlist_items(server_id, removals))
            {
                notify_backend_failure();
                return;
            }
            items = backend_playlist_items(server_id);
            take_backend_error();
        }

        // 4. Append local tracks missing server-side.
        std::set<std::string> server_ids;
        for (const auto &item : items)
            server_ids.insert(item.first);
        std::vector<std::string> additions;
        for (const std::string &id : ids)
        {
            if (!server_ids.count(id))
                additions.push_back(id);
        }
        if (!additions.empty())
        {
            if (!backend_add_playlist_items(server_id, additions))
            {
                notify_backend_failure();
                return;
            }
            // Refetch so new items carry their setVideoId for the reorder.
            items = backend_playlist_items(server_id);
            take_backend_error();
        }

        // 5. Reorder: move every out-of-place item after its desired
        // predecessor (desired[i-1] is already in place from prior moves).
        std::map<std::string, std::string> setvideo_of;
        std::vector<std::string> server_order;
        for (const auto &item : items)
        {
            setvideo_of[item.first] = item.second;
            server_order.push_back(item.first);
        }
        std::vector<std::string> desired;
        for (const std::string &id : ids)
        {
            if (setvideo_of.count(id))
                desired.push_back(id);
        }
        int moves = 0;
        for (size_t i = 0; i < desired.size() && i < server_order.size(); ++i)
        {
            if (server_order[i] == desired[i])
                continue;
            size_t j = i;
            while (j < server_order.size() && server_order[j] != desired[i])
                ++j;
            if (j >= server_order.size())
                break;
            std::string after =
                (i == 0) ? std::string() : setvideo_of[desired[i - 1]];
            if (i != 0 && after.empty())
                break; // setVideoId unknown; retry on the next sync
            if (!backend_move_playlist_item(server_id, setvideo_of[desired[i]], after))
            {
                notify_backend_failure();
                return;
            }
            server_order.erase(server_order.begin() + j);
            server_order.insert(server_order.begin() + i, desired[i]);
            if (++moves > 200)
                break;
        }
    });
}

void wb_rename_playlist(const std::string &old_name, const std::string &new_name)
{
    if (old_name.empty() || new_name.empty() || old_name == new_name ||
        !g_wb_enabled.load())
        return;
    writeback_enqueue([old_name, new_name] {
        std::string old_meta = playlist_meta_path(old_name);
        std::string server_id = read_server_id(old_meta);
        if (server_id.empty())
            return; // playlist was never mirrored
        if (!backend_rename_playlist(server_id, new_name))
        {
            notify_backend_failure();
            return;
        }
        write_server_id(playlist_meta_path(new_name), server_id);
        std::filesystem::remove(old_meta);
    });
}

void wb_delete_playlist(const std::string &name)
{
    if (name.empty() || !g_wb_enabled.load())
        return;
    writeback_enqueue([name] {
        std::string meta = playlist_meta_path(name);
        std::string server_id = read_server_id(meta);
        if (!server_id.empty() && !backend_delete_playlist(server_id))
            notify_backend_failure();
        std::error_code ec;
        std::filesystem::remove(meta, ec);
    });
}
