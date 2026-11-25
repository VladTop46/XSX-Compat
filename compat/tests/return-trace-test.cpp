// Executes the trace thunks with Microsoft x64 register/stack/FP conventions
// on Linux as well as Windows. No original game or graphics dependencies.
#include "../d3d12_xs/return_trace.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace xsx::returns;
static int mode = 1, checked = 0, bad = 0;
static thread_local uint32_t last_error = 0;
static std::mutex mutex;
static std::vector<Event> events;
static int ModeTest() { return mode; }
static uint32_t GetError() { return last_error; }
static void SetError(uint32_t value) { last_error = value; }
static bool ReadWord(uintptr_t address, uintptr_t& value)
{
    last_error = 0xbad;
    if (address < 0x10000) return false;
    std::memcpy(&value, (void*)address, sizeof value);
    return true;
}
static void Emit(const Event& event)
{
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(event);
    last_error = 0xdead; // Observation must not leak its Win32 error state.
}
#define CHECK(x) do { ++checked; if (!(x)) { ++bad; std::printf("[FAIL] line %d: %s\n", __LINE__, #x); } } while (0)
struct State { uint64_t seen = 0; double floating = 0; };
static int32_t XSX_RETURN_ABI Mixed(State* s, double a, float b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, void** out)
{
    s->seen = c + d * 10 + e * 100 + f * 1000 + g * 10000 + h * 100000;
    s->floating = a + b;
    *out = s;
    last_error = 73;
    return (int32_t)0x80070057u;
}
static int32_t XSX_RETURN_ABI NullObject(State*, void** out) { *out = nullptr; last_error = 91; return 0; }
static int32_t XSX_RETURN_ABI ValidateOnly(State*, void**) { return 1; } // S_FALSE, optional ppv
static int32_t XSX_RETURN_ABI Recovered(State* s, void** out)
{
    // A failed internal attempt followed by successful fallback is not a guest failure.
    last_error = 123; *out = s; return 0;
}
static uint32_t XSX_RETURN_ABI ReleaseZero(State*) { return 0; }
static volatile uint64_t* XSX_RETURN_ABI NullPointer(State*) { return nullptr; }
static int32_t XSX_RETURN_ABI SuccessUnwritten(State*, void**) { return 0; }
// Committed127 has eleven arguments, including seven stack arguments. Distinct
// values catch a truncated signature or shifted tail/output on Microsoft x64.
static int32_t XSX_RETURN_ABI Eleven(State* s, const uint64_t* heap, uint32_t flags,
    const uint64_t* desc, uint32_t layout, const uint64_t* clear, void* protectedSession,
    uint32_t count, const uint64_t* formats, const uint64_t* iid, void** out)
{
    s->seen = *heap + flags * 10 + *desc * 100 + layout * 1000 + *clear * 10000 +
        (uintptr_t)protectedSession * 100000 + count * 1000000 + *formats * 10000000 + *iid * 100000000;
    *out = s; last_error = 127; return 0;
}
struct Shape { virtual int32_t XSX_RETURN_ABI GetObject(void**) = 0; };

int main()
{
    hooks = {ModeTest, GetError, SetError, ReadWord, Emit};
    State state; void* output = nullptr;
    void* vt[16]{};
    vt[0] = (void*)&Mixed;
    Install<1, 0>(vt, &Mixed, "Mixed");
    auto mixed = (decltype(&Mixed))vt[0];
    CHECK(mixed(&state, 1.25, 2.5f, 1, 2, 3, 4, 5, 6, &output) == (int32_t)0x80070057u);
    CHECK(state.seen == 654321 && state.floating == 3.75 && output == &state);
    CHECK(last_error == 73 && events.size() == 1);
    CHECK(events.back().finding == Finding::FailedHResult && events.back().caller != 0);
    CHECK(events.back().site.kind == 1 && events.back().site.slot == 0 && events.back().object == (uintptr_t)&state);
    const auto installed_once = installed.load();
    Install<1, 0>(vt, &Mixed, "Mixed");
    CHECK(installed == installed_once); // Reinstallation must never recurse.

    vt[1] = (void*)&NullObject;
    InstallMember<1, 1, State>(vt, &Shape::GetObject, "GetObject");
    CHECK(((decltype(&NullObject))vt[1])(&state, &output) == 0);
    CHECK(events.back().finding == Finding::NullOutput && events.back().output_readable);
    CHECK(last_error == 91);
    vt[2] = (void*)&ValidateOnly;
    Install<1, 2>(vt, &ValidateOnly, "ValidateOnly");
    size_t before = events.size();
    CHECK(((decltype(&ValidateOnly))vt[2])(&state, nullptr) == 1);
    CHECK(((decltype(&ValidateOnly))vt[2])(&state, &output) == 1 && events.size() == before);
    vt[3] = (void*)&Recovered;
    Install<1, 3>(vt, &Recovered, "Recovered");
    CHECK(((decltype(&Recovered))vt[3])(&state, &output) == 0 && output == &state);
    CHECK(events.size() == before && last_error == 123);
    vt[4] = (void*)&ReleaseZero;
    Install<1, 4>(vt, &ReleaseZero, "Release", Contract::ReferenceCount);
    CHECK(((decltype(&ReleaseZero))vt[4])(&state) == 0 && events.size() == before);
    vt[5] = (void*)&NullPointer;
    Install<1, 5>(vt, &NullPointer, "CompletionAddress", Contract::NonNull);
    CHECK(((decltype(&NullPointer))vt[5])(&state) == nullptr);
    CHECK(events.back().finding == Finding::NullReturn);
    vt[6] = (void*)&SuccessUnwritten;
    Install<1, 6>(vt, &SuccessUnwritten, "Unwritten");
    CHECK(((decltype(&SuccessUnwritten))vt[6])(&state, (void**)0x1000) == 0);
    CHECK(events.back().finding == Finding::UnreadableOutput);

    vt[8] = (void*)&Eleven;
    Install<1, 8>(vt, &Eleven, "Committed127");
    uint64_t heap = 1, desc = 3, clear = 5, formats = 8, iid = 9;
    mode = 2;
    CHECK(((decltype(&Eleven))vt[8])(&state, &heap, 2, &desc, 4, &clear,
        (void*)6, 7, &formats, &iid, &output) == 0);
    CHECK(state.seen == 987654321 && output == &state && last_error == 127);
    CHECK(events.back().finding == Finding::None && events.back().output_value == (uintptr_t)&state);
    mode = 1;

    mode = 0;
    vt[7] = (void*)&NullObject;
    Install<1, 7>(vt, &NullObject, "Off");
    CHECK(vt[7] == (void*)&NullObject);
    before = events.size();
    CHECK(((decltype(&NullObject))vt[1])(&state, &output) == 0 && events.size() == before);
    mode = 2;
    CHECK(((decltype(&ReleaseZero))vt[4])(&state) == 0);
    CHECK(events.back().finding == Finding::None && events.back().value == 0);

    mode = 1;
    const uint64_t count_before = calls, fail_before = failures;
    const size_t events_before = events.size();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) threads.emplace_back([&] {
        State s; void* out;
        for (int i = 0; i < 1000; ++i) mixed(&s, 1.25, 2.5f, 1, 2, 3, 4, 5, 6, &out);
    });
    for (auto& thread : threads) thread.join();
    CHECK(calls == count_before + 8000 && returned == calls);
    CHECK(failures == fail_before + 8000 && events.size() == events_before + 8000);
    std::vector<bool> unique(8000, false);
    for (size_t i = events_before; i < events.size(); ++i) {
        uint64_t n = events[i].failure - fail_before - 1;
        if (n >= unique.size() || unique[n]) ++bad;
        else unique[n] = true;
    }
    CHECK(!bad);
    std::printf("[%s] return trace checks=%d failures=%d ms_x64_mixed_args=1 threaded_calls=8000\n", bad ? "FAIL" : "PASS", checked, bad);
    return bad ? 1 : 0;
}
