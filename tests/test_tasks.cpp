// Stress the worker registry: spawn many short tasks from one thread while
// they finish concurrently. This used to deadlock when a worker published its
// `finished` flag before releasing the registry mutex, because spawn() joined
// it from inside that same mutex.

#include "tasks.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main()
{
    std::atomic<bool> done{false};
    std::thread watchdog([&done]() {
        for (int i = 0; i < 200; ++i)
        {
            if (done.load(std::memory_order_acquire))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        std::fputs("tasks stress test timed out (possible deadlock)\n", stderr);
        std::fflush(stderr);
        std::_Exit(2);
    });

    for (int i = 0; i < 20000; ++i)
        tasks::spawn([]() {});

    tasks::shutdown();
    done.store(true, std::memory_order_release);
    watchdog.join();

    std::puts("tasks stress test: ok");
    return 0;
}
