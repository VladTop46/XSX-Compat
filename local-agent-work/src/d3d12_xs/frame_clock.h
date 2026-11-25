#pragma once

// Bootstrap frame events, independent of a display or GPU. The GDK sample
// supplies intervals in microseconds. Local title calls corroborate origin0,
// flip2 and TOKEN_ON_TIMEOUT2. Only origin creates frame tokens. This clock
// does not claim scanout timestamps, GPU-triggered events or frame statistics.
#include <array>
#include <cstdint>
#include <mutex>

namespace xsx::frame {
constexpr int32_t kOk = 0, kTimeout = 1; // S_OK / S_FALSE
constexpr int32_t kInvalidArgument = static_cast<int32_t>(0x80070057u);
constexpr int32_t kNotImplemented = static_cast<int32_t>(0x80004001u);
constexpr uint32_t kOrigin = 0, kFlip = 2, kTokenOnTimeout = 2;
constexpr uint32_t kInfinite = UINT32_MAX;

struct Result {
    int32_t status = kOk;
    bool token_created = false;
    uint64_t token = 0, tick = 0, wait_ns = 0;
};
struct State {
    uint32_t interval_us = 16667, period = 1;
    uint64_t frames = 0;
    std::array<uint64_t, 3> reserved{};
};

class Timeline {
    std::mutex mutex_;
    State state_;
    bool started_ = false;
    uint64_t epoch_ns_ = 0, first_tick_ = 0;

    static uint64_t Add(uint64_t a, uint64_t b) {
        return b > UINT64_MAX - a ? UINT64_MAX : a + b;
    }
    void Start(uint64_t now) {
        started_ = true;
        epoch_ns_ = now;
        first_tick_ = 0;
        for (auto tick : state_.reserved) if (tick > first_tick_) first_tick_ = tick;
    }
public:
    void SetInterval(uint64_t now, uint32_t microseconds, uint32_t period) {
        std::lock_guard<std::mutex> held(mutex_);
        microseconds = microseconds ? microseconds : 16667;
        // Repeated identical settings must not move an absolute clock's phase.
        if (!started_ || microseconds != state_.interval_us) Start(now);
        state_.interval_us = microseconds;
        state_.period = period; // Pipeline depth, not a multiplier of the rate.
    }

    // Clock supplies monotonic NowNs() and SleepUntilNs(absolute deadline).
    // Reserve a distinct event tick under the lock, then release it before
    // sleeping: a zero-timeout origin poll must not wait behind a flip sleep.
    template<class Clock>
    Result Wait(Clock& clock, uint32_t kind, uint32_t timeout_ms, uint32_t flags) {
        if (kind >= state_.reserved.size()) return {kInvalidArgument};
        if (flags & ~kTokenOnTimeout) return {kNotImplemented};
        Result result;
        uint64_t end;
        {
            std::lock_guard<std::mutex> held(mutex_);
            const uint64_t now = clock.NowNs();
            if (!started_) Start(now);
            const uint64_t interval = uint64_t(state_.interval_us) * 1000;
            const uint64_t elapsed = now >= epoch_ns_ ? now - epoch_ns_ : 0;
            uint64_t step = elapsed / interval + 1;
            uint64_t tick = Add(first_tick_, step);
            if (tick <= state_.reserved[kind]) tick = Add(state_.reserved[kind], 1);
            step = tick - first_tick_;
            const uint64_t target = step > UINT64_MAX / interval ? UINT64_MAX : Add(epoch_ns_, step * interval);
            const uint64_t limit = timeout_ms == kInfinite ? UINT64_MAX : Add(now, uint64_t(timeout_ms) * 1000000);
            result.tick = tick;
            if (target > limit) {
                result.status = kTimeout;
                end = limit; // A timeout consumes no event tick.
            } else {
                state_.reserved[kind] = tick;
                end = target;
            }
            result.wait_ns = end > now ? end - now : 0;
        }
        if (result.wait_ns) clock.SleepUntilNs(end);
        if (kind == kOrigin && (result.status == kOk || (flags & kTokenOnTimeout))) {
            std::lock_guard<std::mutex> held(mutex_);
            result.token_created = true;
            result.token = ++state_.frames;
        }
        return result;
    }

    State Snapshot() {
        std::lock_guard<std::mutex> held(mutex_);
        return state_;
    }
    bool TrySnapshot(State& out) {
        std::unique_lock<std::mutex> held(mutex_, std::try_to_lock);
        if (!held.owns_lock()) return false;
        out = state_;
        return true;
    }
};
} // namespace xsx::frame
