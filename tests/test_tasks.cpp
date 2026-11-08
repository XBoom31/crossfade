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

// ThreadSanitizer makes creating tens of thousands of short-lived threads
// extremely slow. Keep the normal/ASan stress count, but shrink the TSan run
// so the watchdog measures a deadlock rather than sanitizer overhead.
#if defined(__SANITIZE_THREAD__)
#define CF_STRESS_TASKS 2000
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CF_STRESS_TASKS 2000
#else
#define CF_STRESS_TASKS 20000
#endif
#else
#define CF_STRESS_TASKS 20000
#endif

int main()
{
    // A real deadlock still fails the test, just after a generous window.
    constexpr int kWatchdogTicks = 1200; // 1200 * 25 ms = 30 s
    std::atomic<bool> done{false};
    std::thread watchdog([&done]() {
        for (int i = 0; i < kWatchdogTicks; ++i)
        {
            if (done.load(std::memory_order_acquire))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        std::fputs("tasks stress test timed out (possible deadlock)\n", stderr);
        std::fflush(stderr);
        std::_Exit(2);
    });

    for (int i = 0; i < CF_STRESS_TASKS; ++i)
        tasks::spawn([]() {});

    tasks::shutdown();
    done.store(true, std::memory_order_release);
    watchdog.join();

    std::puts("tasks stress test: ok");
    return 0;
}
