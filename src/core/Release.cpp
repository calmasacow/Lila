#include "core/Release.h"

#include "core/Profile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lila::release
{
namespace
{
struct State
{
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<std::unique_ptr<Piece>> queue;
    size_t busy = 0;
    std::atomic<int> holds{0};
    bool started = false;
};

/** Never destroyed: pieces may still be queued when statics are torn down at exit. */
State& state()
{
    static State* s = new State;
    return *s;
}

void run()
{
    LILA_THREAD_NAME("lila releaser");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    State& s = state();
    for (;;)
    {
        std::unique_ptr<Piece> p;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [&] { return !s.queue.empty(); });
            p = std::move(s.queue.front());
            s.queue.pop_front();
            ++s.busy;
        }
        for (bool more = true; more;)
        {
            while (s.holds.load(std::memory_order_acquire) > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            LILA_ZONE_N("release.step");
            more = p->step();
        }
        p.reset();
        std::scoped_lock lock(s.mutex);
        --s.busy;
        if (s.queue.empty() && s.busy == 0)
            s.idle.notify_all();
    }
}
} // namespace

void enqueue(std::unique_ptr<Piece> piece)
{
    if (!piece)
        return;
    State& s = state();
    {
        std::scoped_lock lock(s.mutex);
        s.queue.push_back(std::move(piece));
        if (!s.started)
        {
            s.started = true;
            std::thread(run).detach();
        }
    }
    s.wake.notify_one();
}

Hold::Hold() { state().holds.fetch_add(1, std::memory_order_acq_rel); }
Hold::~Hold() { state().holds.fetch_sub(1, std::memory_order_acq_rel); }

void drain()
{
    State& s = state();
    std::unique_lock lock(s.mutex);
    s.idle.wait(lock, [&] { return s.queue.empty() && s.busy == 0; });
}

size_t pending()
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    return s.queue.size() + s.busy;
}
} // namespace lila::release
