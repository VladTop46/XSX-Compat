// Deterministic timing/timeout regression, no Wine, GPU or title data.
#include "../d3d12_xs/frame_clock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include <vector>
using namespace xsx::frame;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
struct Clock {
    std::atomic<uint64_t> now{0}, sleeps{0};
    uint64_t NowNs() { return now.load(); }
    void SleepUntilNs(uint64_t at) {
        ++sleeps;
        uint64_t old = now.load();
        while (at > old && !now.compare_exchange_weak(old, at)) {}
    }
};
struct PausedClock : Clock {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, resume = false;
    void SleepUntilNs(uint64_t at) {
        std::unique_lock<std::mutex> held(mutex);
        entered = true;
        cv.notify_one();
        cv.wait(held, [&] { return resume; });
        Clock::SleepUntilNs(at);
    }
};
int main()
{
    {
        Timeline timeline; Clock clock;
        timeline.SetInterval(0, 33333, 3);
        auto r = timeline.Wait(clock, kOrigin, 0, 0);
        CHECK(r.status == kTimeout && !r.token_created);
        CHECK(clock.NowNs() == 0 && clock.sleeps == 0);
        r = timeline.Wait(clock, kOrigin, 0, kTokenOnTimeout);
        CHECK(r.status == kTimeout && r.token_created && r.token == 1);
        r = timeline.Wait(clock, kOrigin, 0, kTokenOnTimeout);
        CHECK(r.status == kTimeout && r.token == 2 && clock.NowNs() == 0);
        r = timeline.Wait(clock, kFlip, kInfinite, 0);
        CHECK(r.status == kOk && !r.token_created && clock.NowNs() == 33333000);
        CHECK(timeline.Snapshot().frames == 2);
        for (unsigned frame = 3; frame <= 14; ++frame) {
            r = timeline.Wait(clock, kOrigin, 0, kTokenOnTimeout);
            CHECK(r.status == kTimeout && r.token_created && r.token == frame);
            r = timeline.Wait(clock, kFlip, kInfinite, 0);
            CHECK(r.status == kOk && !r.token_created);
            CHECK(clock.NowNs() == uint64_t(frame - 1) * 33333000);
        }
        CHECK(timeline.Snapshot().frames == 14);
        CHECK(timeline.Snapshot().period == 3);
    }
    {
        Timeline timeline; Clock clock;
        timeline.SetInterval(0, 10000, 1);
        auto r = timeline.Wait(clock, kOrigin, 5, 0);
        CHECK(r.status == kTimeout && !r.token_created && clock.NowNs() == 5000000);
        r = timeline.Wait(clock, kOrigin, 2, kTokenOnTimeout);
        CHECK(r.status == kTimeout && r.token == 1 && clock.NowNs() == 7000000);
        r = timeline.Wait(clock, kOrigin, 3, 0);
        CHECK(r.status == kOk && r.token == 2 && clock.NowNs() == 10000000);
        CHECK(timeline.Snapshot().reserved[0] == 1); // Timeouts consumed no tick.
        r = timeline.Wait(clock, 1, 10, 0);
        CHECK(r.status == kOk && !r.token_created && clock.NowNs() == 20000000);
        r = timeline.Wait(clock, kFlip, kInfinite, kTokenOnTimeout);
        CHECK(!r.token_created && timeline.Snapshot().frames == 2);
    }
    {
        Timeline timeline; Clock clock;
        timeline.SetInterval(0, 5000, 3);
        auto r = timeline.Wait(clock, kFlip, kInfinite, 0);
        CHECK(r.status == kOk && clock.NowNs() == 5000000);
        clock.now = 6500000;
        timeline.SetInterval(clock.NowNs(), 5000, 9); // Same interval keeps phase.
        r = timeline.Wait(clock, kFlip, kInfinite, 0);
        CHECK(r.status == kOk && clock.NowNs() == 10000000);
        clock.now = 17750000; // A late caller uses the absolute next tick.
        r = timeline.Wait(clock, kFlip, kInfinite, 0);
        CHECK(r.status == kOk && clock.NowNs() == 20000000);
        timeline.SetInterval(clock.NowNs(), 2000, 100);
        r = timeline.Wait(clock, kOrigin, kInfinite, 0);
        CHECK(r.status == kOk && r.token == 1 && clock.NowNs() == 22000000);
        timeline.SetInterval(clock.NowNs(), 0, 0);
        CHECK(timeline.Snapshot().interval_us == 16667);
        r = timeline.Wait(clock, kFlip, kInfinite, 0);
        CHECK(r.status == kOk && clock.NowNs() == 38667000);
    }
    {
        Timeline timeline; Clock clock;
        CHECK(timeline.Wait(clock, 3, 0, 0).status == kInvalidArgument);
        CHECK(timeline.Wait(clock, kOrigin, 0, 1).status == kNotImplemented);
        CHECK(timeline.Wait(clock, kOrigin, 0, 3).status == kNotImplemented);
        CHECK(clock.sleeps == 0 && timeline.Snapshot().frames == 0);
        timeline.SetInterval(0, UINT32_MAX, 1);
        const auto r = timeline.Wait(clock, kFlip, kInfinite, 0);
        CHECK(r.status == kOk && clock.NowNs() == uint64_t(UINT32_MAX) * 1000);
    }
    {
        // A long flip wait must not block an origin poll on the clock lock.
        Timeline timeline; PausedClock clock;
        timeline.SetInterval(0, 100000, 1);
        Result flip;
        std::thread waiter([&] { flip = timeline.Wait(clock, kFlip, kInfinite, 0); });
        {
            std::unique_lock<std::mutex> held(clock.mutex);
            CHECK(clock.cv.wait_for(held, std::chrono::seconds(2), [&] { return clock.entered; }));
        }
        const auto origin = timeline.Wait(clock, kOrigin, 0, kTokenOnTimeout);
        CHECK(origin.status == kTimeout && origin.token == 1 && clock.NowNs() == 0);
        {
            std::lock_guard<std::mutex> held(clock.mutex);
            clock.resume = true;
        }
        clock.cv.notify_one();
        waiter.join();
        CHECK(flip.status == kOk && !flip.token_created && clock.NowNs() == 100000000);
    }
    {
        Timeline timeline; Clock clock;
        std::vector<uint64_t> tokens(8000);
        std::atomic<unsigned> errors{0};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 8; ++t) threads.emplace_back([&, t] {
            for (unsigned n = 0; n < 1000; ++n) {
                auto r = timeline.Wait(clock, kOrigin, 0, kTokenOnTimeout);
                if (r.status != kTimeout || !r.token_created) ++errors;
                tokens[t * 1000 + n] = r.token;
            }
        });
        for (auto& thread : threads) thread.join();
        std::sort(tokens.begin(), tokens.end());
        bool consecutive = true;
        for (size_t i = 0; i < tokens.size(); ++i) consecutive &= tokens[i] == i + 1;
        CHECK(errors == 0 && consecutive);
        CHECK(timeline.Snapshot().frames == 8000 && clock.sleeps == 0);
    }
    std::printf("[%s] frame clock checks=%d failures=%d concurrent_tokens=8000\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
