#include "tasks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tasks
{
namespace
{
struct Record
{
    uint64_t id = 0;
    std::string name;
    bool active = true;
    double start_ms = 0.0;
    double duration_ms = 0.0;
};

struct Entry
{
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
    std::shared_ptr<Record> record;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<Entry> g_workers;
std::deque<std::shared_ptr<Record>> g_records;
size_t g_active = 0;
uint64_t g_total = 0;
uint64_t g_next_id = 1;
bool g_shutting_down = false;
bool g_history_enabled = true;

constexpr size_t kMaxRecords = 256;

double clock_ms()
{
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() /
           1000.0;
}

void prune_finished_locked()
{
    for (auto it = g_workers.begin(); it != g_workers.end();)
    {
        if (!it->finished || !it->finished->load())
        {
            ++it;
            continue;
        }
        if (it->thread.joinable())
            it->thread.join();
        it = g_workers.erase(it);
    }
}
} // namespace

void spawn(std::function<void()> fn) { spawn("task", std::move(fn)); }

void spawn(const char *name, std::function<void()> fn)
{
    if (!fn)
        return;

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutting_down)
        return;
    prune_finished_locked();

    auto finished = std::make_shared<std::atomic<bool>>(false);
    auto record = std::make_shared<Record>();
    record->id = g_next_id++;
    record->name = (name && *name) ? name : "task";
    record->start_ms = clock_ms();
    ++g_total;
    if (g_history_enabled)
    {
        g_records.push_back(record);
        while (g_records.size() > kMaxRecords)
            g_records.pop_front();
    }

    ++g_active;
    Entry entry;
    entry.finished = finished;
    entry.record = record;
    entry.thread = std::thread([fn = std::move(fn), finished, record]() mutable {
        fn();
        // Decrement while holding g_mu *before* publishing `finished`. spawn()
        // prunes under g_mu and joins any entry whose `finished` flag is set;
        // setting it first let the main thread join this worker while the
        // worker was still waiting for g_mu, deadlocking the UI forever.
        {
            std::lock_guard<std::mutex> done_lock(g_mu);
            --g_active;
            record->active = false;
            record->duration_ms = clock_ms() - record->start_ms;
        }
        finished->store(true);
        g_cv.notify_all();
    });
    g_workers.push_back(std::move(entry));
}

void shutdown()
{
    std::vector<Entry> pending;
    {
        std::unique_lock<std::mutex> lock(g_mu);
        g_shutting_down = true;
        // Give in-flight network/cache work a bounded drain. Backend calls
        // have their own timeouts and stop_backend() closes the pipes before
        // this is called, so most workers finish almost immediately.
        g_cv.wait_for(lock, std::chrono::seconds(8), [] { return g_active == 0; });
        pending.swap(g_workers);
    }

    for (auto &e : pending)
    {
        const bool done = e.finished && e.finished->load();
        if (done && e.thread.joinable())
            e.thread.join();
        else if (e.thread.joinable())
            e.thread.detach();
    }
}

std::vector<Snapshot> snapshot()
{
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<Snapshot> out;
    out.reserve(g_records.size());
    for (const auto &r : g_records)
    {
        Snapshot s;
        s.id = r->id;
        s.name = r->name;
        s.active = r->active;
        s.start_ms = r->start_ms;
        s.duration_ms = r->duration_ms;
        out.push_back(std::move(s));
    }
    return out;
}

size_t active_count()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_active;
}

uint64_t total_count()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_total;
}

void set_history_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_history_enabled = enabled;
    if (!enabled)
        g_records.clear();
}
} // namespace tasks
