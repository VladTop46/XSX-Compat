// CPU-only x64 fixture: native execution versus persistent instruction traps.
// No title code, title files, graphics device or network is needed.
#include "../d3d12_xs/persistent_trace.h"
#include <cstdio>
#include <cstdint>
#include <cstddef>

using xsx::diagnostic::PersistentTrace;
using xsx::diagnostic::Point;
using xsx::diagnostic::Instruction;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
struct Owner { unsigned char pad[0x78]{}; uint32_t generation = 0; uint8_t queued = 0; unsigned char unused[3]{}; uint32_t pending = 0; };
struct Frame { unsigned char pad[0x1548]{}; uint32_t count = 0; };
struct Task { unsigned char pad[0x20]{}; Owner* owner = nullptr; Frame* frame = nullptr; uint32_t expected = 0; };
static_assert(offsetof(Owner,generation) == 0x78 && offsetof(Owner,queued) == 0x7c && offsetof(Owner,pending) == 0x80);
static_assert(offsetof(Task,owner) == 0x20 && offsetof(Task,frame) == 0x28 && offsetof(Task,expected) == 0x30);
using Job = void (WINAPI*)(Task*);
static constexpr unsigned workers = 8, iterations = 2000;
static std::atomic<unsigned> observerErrors{0};
static thread_local DWORD casFlags = 0;
static void Observe(unsigned point, const CONTEXT& context) noexcept
{
    constexpr DWORD arithmetic = 0x8d5; // CF/PF/AF/ZF/SF/OF
    if (point == 1) casFlags = context.EFlags & arithmetic;
    if (point == 2 && (context.EFlags & arithmetic) != casFlags) ++observerErrors;
    // The trap engine must restore the interrupted thread's last-error value.
    SetLastError(0xaabbccdd);
}
struct Worker {
    Owner owner; Frame frame; Task task;
    Job job = nullptr; HANDLE start = nullptr;
    unsigned errors = 0, ordinal = 0;
};
static DWORD WINAPI Work(void* opaque)
{
    auto& worker = *(Worker*)opaque;
    if (WaitForSingleObject(worker.start, 10000) != WAIT_OBJECT_0) { ++worker.errors; return 1; }
    for (unsigned i = 0; i < iterations; ++i) {
        const bool accept = !(i & 1);
        const uint32_t before = worker.owner.generation;
        worker.task.expected = before + (accept ? 0 : 99);
        worker.owner.queued = 1;
        const DWORD marker = 0x12340000 + worker.ordinal;
        SetLastError(marker);
        worker.job(&worker.task);
        if (GetLastError() != marker || worker.owner.queued ||
            worker.owner.generation != before + (accept ? 1 : 0) ||
            worker.frame.count != (i + 2) / 2 || worker.owner.pending != iterations - i - 1) ++worker.errors;
    }
    return 0;
}
static bool Run(Job job, const char* phase)
{
    std::array<Worker,workers> data{};
    std::array<HANDLE,workers> threads{};
    HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr); CHECK(start);
    if (!start) return false;
    unsigned created = 0;
    for (unsigned i = 0; i < workers; ++i) {
        auto& worker = data[i]; worker.ordinal = i;
        worker.owner.generation = 17 + i; worker.owner.pending = iterations;
        worker.task.owner = &worker.owner; worker.task.frame = &worker.frame;
        worker.job = job; worker.start = start;
        threads[i] = CreateThread(nullptr, 0, Work, &worker, 0, nullptr); CHECK(threads[i]);
        if (!threads[i]) break;
        ++created;
    }
    SetEvent(start);
    const DWORD waited = created ? WaitForMultipleObjects(created, threads.data(), TRUE, 30000) : WAIT_FAILED;
    CHECK(waited == WAIT_OBJECT_0);
    if (waited != WAIT_OBJECT_0) ExitProcess(1); // Workers still own stack/code.
    unsigned errors = 0;
    for (unsigned i = 0; i < created; ++i) { errors += data[i].errors; CloseHandle(threads[i]); }
    CloseHandle(start);
    CHECK(created == workers); CHECK(errors == 0);
    std::printf("[TASK TRACE] phase=%s workers=%u calls=%u errors=%u\n",phase,created,created*iterations,errors);
    return created == workers && errors == 0;
}
int main()
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto* code = (unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); CHECK(code);
    if (!code) return 1;
    // Handwritten test routine. Its shared entry performs a generation CAS,
    // conditionally calls the update, and always clears queued/decrements pending.
    const unsigned char wrapper[]{
        0x56,0x48,0x83,0xec,0x20,
        0x48,0x89,0xc8,                    // site0: mov rax,rcx
        0x48,0x8b,0x71,0x20,0x48,0x8b,0x49,0x28,
        0x8b,0x40,0x30,0x8d,0x50,0x01,
        0xf0,0x0f,0xb1,0x56,0x78,
        0x75,0x05,                         // site1: jnz +5
        0xe8,0,0,0,0,
        0xc6,0x46,0x7c,0,
        0xf0,0xff,0x8e,0x80,0,0,0,
        0x48,0x83,0xc4,0x20,0x5e,0xc3,
    };
    const unsigned char update[]{
        0x56,0x48,0x89,0xce,                // site2: mov rsi,rcx
        0x31,0xc9,                         // clobber rcx: site3 must restore it
        0xff,0x86,0x48,0x15,0,0,
        0x48,0x89,0xf1,                    // site3: mov rcx,rsi
        0x8b,0x81,0x48,0x15,0,0,
        0x5e,0xc3,
    };
    std::memcpy(code,wrapper,sizeof wrapper); std::memcpy(code+64,update,sizeof update);
    const int32_t relative = 64 - 34; std::memcpy(code+30,&relative,4);
    DWORD old = 0; CHECK(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
    CHECK(FlushInstructionCache(GetCurrentProcess(),code,4096));
    const auto job = reinterpret_cast<Job>(code);
    if (!Run(job,"native")) return 1;
    const auto base = (uintptr_t)code;
    const std::array<Point,4> points{{
        {base+5,Instruction::RaxFromRcx},{base+27,Instruction::JnzPlusFive},
        {base+65,Instruction::RsiFromRcx},{base+76,Instruction::RcxFromRsi},
    }};
    PersistentTrace trace;
    auto wrong = points; wrong[0].instruction = Instruction::RsiFromRcx;
    CHECK(trace.Install(wrong) == E_INVALIDARG && !trace.Active());
    CHECK(code[5] == 0x48 && code[27] == 0x75);
    CHECK(trace.Install(points,Observe) == S_OK && trace.Active());
    if (!trace.Active()) return 1;
    for (const auto& point : points) CHECK(*(unsigned char*)point.address == 0xcc);
    Run(job,"persistent-traps");
    CHECK(trace.Intact());
    CHECK(trace.Hits(0) == workers*iterations); CHECK(trace.Hits(1) == workers*iterations);
    CHECK(trace.Hits(2) == workers*iterations/2); CHECK(trace.Hits(3) == workers*iterations/2);
    CHECK(observerErrors.load() == 0);
    std::printf("[TASK TRACE] entry=%llu post_cas=%llu update=%llu after_increment=%llu flags_errors=%u\n",
        (unsigned long long)trace.Hits(0),(unsigned long long)trace.Hits(1),
        (unsigned long long)trace.Hits(2),(unsigned long long)trace.Hits(3),observerErrors.load());
    EXCEPTION_RECORD record{}; CONTEXT context{}; EXCEPTION_POINTERS exception{&record,&context};
    record.ExceptionCode = EXCEPTION_BREAKPOINT; record.ExceptionAddress = (void*)points[0].address;
    context.Rip = points[0].address; CHECK(trace.Owns(&exception));
    context.Rip = points[0].address+1; CHECK(trace.Owns(&exception));
    record.ExceptionAddress = code; CHECK(!trace.Owns(&exception));
    record.ExceptionAddress = (void*)points[0].address; record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    CHECK(!trace.Owns(&exception));
    CHECK(trace.Uninstall() && !trace.Active());
    CHECK(!std::memcmp(code,wrapper,30)); // Call displacement was filled after wrapper copy.
    CHECK(!std::memcmp(code+64,update,sizeof update));
    VirtualFree(code,0,MEM_RELEASE);
    std::printf("[%s] task trace checks=%u failures=%u concurrent_calls=%u (CPU only)\n",failures?"FAIL":"PASS",checks,failures,workers*iterations);
    return failures?1:0;
}
