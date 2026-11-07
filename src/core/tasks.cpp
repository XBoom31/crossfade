#include "tasks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tasks
{
namespace
{
struct Entry
{
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<Entry> g_workers;
size_t g_active = 0;
bool g_shutting_down = false;

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

void spawn(std::function<void()> fn)
{
    if (!fn)
        return;

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutting_down)
        return;
    prune_finished_locked();

    auto finished = std::make_shared<std::atomic<bool>>(false);
    ++g_active;
    Entry entry;
    entry.finished = finished;
    entry.thread = std::thread([fn = std::move(fn), finished]() mutable {
        fn();
        // Decrement while holding g_mu *before* publishing `finished`. spawn()
        // prunes under g_mu and joins any entry whose `finished` flag is set;
        // setting it first let the main thread join this worker while the
        // worker was still waiting for g_mu, deadlocking the UI forever.
        {
            std::lock_guard<std::mutex> done_lock(g_mu);
            --g_active;
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
} // namespace tasks
