#pragma once
#include "persistent_trace.h"

// Build-specific, opt-in observation only. The executable's four instruction
// byte sequences must match before any patch is written. No task is scheduled,
// no generation is changed, and no writer/allocator data is written.
static xsx::diagnostic::PersistentTrace g_writerTasks;
struct WriterTaskRecord {
    std::atomic<uintptr_t> object{0}, owner{0};
    std::atomic<UINT64> entries{0}, accepted{0}, rejected{0}, updates{0}, finished{0};
};
static WriterTaskRecord g_writerTaskRecords[32];
static std::atomic<UINT64> g_writerTaskUnrecorded{0};
template<class T> static bool ReadTaskField(uintptr_t address, T& value)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void*)address, &value, sizeof value, &read) && read == sizeof value;
}
// Read-only state of the audio object identified by the task and registry gate.
// Layout is corroborated by this executable's lock enter/leave, constructor,
// producer-readiness check (5594650) and output poll (43ae0f0). A snapshot is
// not atomic across fields; no lock, semaphore or title callback is changed.
static void WriterAudioState(const char* phase, uintptr_t object)
{
    if (!object) return;
    const uintptr_t lock = object + 0x3ced0;
    std::array<unsigned char,32> fields{};
    const bool lockReadable = ReadTaskField(lock, fields);
    uint64_t state = 0, semaphore = 0;
    uint32_t recursion = 0, spins = 0;
    std::memcpy(&state, fields.data(), 8); std::memcpy(&semaphore, fields.data()+8, 8);
    std::memcpy(&recursion, fields.data()+0x10, 4); std::memcpy(&spins, fields.data()+0x1c, 4);
    const uint32_t owner = (uint32_t)(state >> 32);
    // The game's thread-ID fallback is a direct GetCurrentThreadId thunk.
    // An unreadable/free lock is not used as an OpenThread input.
    int ownerLive = -1;
    if (lockReadable && owner != UINT32_MAX && owner) {
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, owner);
        if (thread) {
            DWORD code = 0;
            if (GetProcessIdOfThread(thread) == GetCurrentProcessId() && GetExitCodeThread(thread, &code))
                ownerLive = code == STILL_ACTIVE ? 1 : 0;
            CloseHandle(thread);
        }
    }
    XS_LOG("AUDIO LOCK phase=%s this=%p lock=%p readable=%u state=%#llx owner_tid=%u owner_live=%d low32=%u recursion=%u semaphore=%#llx spins=%u",
        phase, (void*)object, (void*)lock, lockReadable, (unsigned long long)state, owner, ownerLive,
        (uint32_t)state, recursion, (unsigned long long)semaphore, spins);
    uintptr_t table = 0, endUpdate = 0, output = 0;
    uint64_t flags = 0, loopFlags = 0;
    uint32_t rings = 0, outputState = 0;
    const bool objectReadable = ReadTaskField(object, table) && table && ReadTaskField(table + 0xa8, endUpdate);
    const bool flagsReadable = ReadTaskField(object + 0x3cdf8, flags) &&
        ReadTaskField(object + 0x3cea8, loopFlags) && ReadTaskField(object + 0x3dd80, outputState);
    const bool outputReadable = ReadTaskField(object + 0x3ce38, output) && ReadTaskField(lock + 0x52c, rings);
    char where[160]; CallerStr((void*)endUpdate, where, sizeof where);
    XS_LOG("AUDIO STATE phase=%s object_readable=%u table=%p end_update=%s flags_readable=%u flags_cdf8=%#llx flags_cea8=%#llx output_state_dd80=%#x output_readable=%u output=%p rings=%u",
        phase, objectReadable, (void*)table, where, flagsReadable, (unsigned long long)flags,
        (unsigned long long)loopFlags, outputState, outputReadable, (void*)output, rings);
    // First two ring descriptors only; report raw counters rather than invent
    // progress when the status pointer or descriptor cannot be read.
    for (unsigned i = 0; outputReadable && i < rings && i < 2; ++i) {
        const uintptr_t descriptor = lock + 0x530 + i * 0x58;
        uint32_t wordC = 0, count = 0, statusWord8 = 0; uintptr_t data = 0;
        const bool descriptorReadable = ReadTaskField(descriptor + 0x0c, wordC) &&
            ReadTaskField(descriptor + 0x34, count) && ReadTaskField(descriptor + 0x38, data);
        const bool statusReadable = descriptorReadable && count && data && ReadTaskField(data + 8, statusWord8);
        XS_LOG("AUDIO RING phase=%s index=%u descriptor=%p readable=%u word_c=%u count=%u data=%p status_readable=%u status_word8=%u",
            phase, i, (void*)descriptor, descriptorReadable, wordC, count, (void*)data, statusReadable, statusWord8);
    }
}
static WriterTaskRecord* WriterTaskFor(uintptr_t object)
{
    if (!object) { ++g_writerTaskUnrecorded; return nullptr; }
    for (auto& record : g_writerTaskRecords) {
        uintptr_t key = record.object.load();
        if (key == object) return &record;
        if (!key && record.object.compare_exchange_strong(key, object)) return &record;
        if (key == object) return &record;
    }
    ++g_writerTaskUnrecorded; return nullptr;
}
static void ObserveWriterTask(unsigned point, const CONTEXT& context) noexcept
{
    uintptr_t task = 0, object = 0, owner = 0, caller = 0;
    uint32_t expected = 0, observed = 0, rawFrame = 0;
    const bool accepted = (context.EFlags & 0x40) != 0;
    if (point == 0) {
        task = context.Rcx;
        ReadTaskField(task + 0x20, owner); ReadTaskField(task + 0x28, object);
        ReadTaskField(task + 0x30, expected);
        if (owner) ReadTaskField(owner + 0x78, observed);
    } else if (point == 1) {
        owner = context.Rsi; object = context.Rcx;
        expected = (uint32_t)context.Rdx - 1; observed = (uint32_t)context.Rax;
    } else object = point == 2 ? context.Rcx : context.Rsi;
    auto* record = WriterTaskFor(object);
    if (record) {
        if (owner) record->owner.store(owner);
        if (point == 0) ++record->entries;
        else if (point == 1) { if (accepted) ++record->accepted; else ++record->rejected; }
        else if (point == 2) ++record->updates;
        else ++record->finished;
    }
    // Bound detailed logging; counters keep running for every hit/thread.
    if (g_writerTasks.Hits(point) > 8) return;
    const bool frameReadable = object && ReadTaskField(object + 0x1548, rawFrame);
    ReadTaskField(context.Rsp + (point < 2 ? 0x28 : 0x88), caller);
    char where[160]; CallerStr((void*)caller, where, sizeof where);
    static const char* names[]{"entry","post-cas","update","after-increment"};
    std::fprintf(stderr, "[D3D12_XS] TASK point=%s hit=%llu tid=%lu task=%p this=%p owner=%p expected=%u observed=%u zf=%u raw_1548=%u frame_readable=%u caller=%s\n",
        names[point], (unsigned long long)g_writerTasks.Hits(point), GetCurrentThreadId(),
        (void*)task, (void*)object, (void*)owner, expected, observed, accepted, rawFrame, frameReadable, where);
    std::fflush(stderr);
    WriterAudioState(names[point], object);
}
static bool IsWriterTaskBreakpoint(const EXCEPTION_POINTERS* exception)
{
    return g_writerTasks.Owns(exception);
}
static void WriterTaskSnapshot()
{
    if (!g_writerTasks.Active()) return;
    XS_LOG("TASK SUMMARY entry=%llu post_cas=%llu update=%llu after_increment=%llu unrecorded=%llu intact=%u (persistent instruction traps)",
        (unsigned long long)g_writerTasks.Hits(0), (unsigned long long)g_writerTasks.Hits(1),
        (unsigned long long)g_writerTasks.Hits(2), (unsigned long long)g_writerTasks.Hits(3),
        (unsigned long long)g_writerTaskUnrecorded.load(), g_writerTasks.Intact());
    for (const auto& record : g_writerTaskRecords) {
        const auto object = record.object.load(); if (!object) continue;
        const auto owner = record.owner.load(); uint32_t generation = 0, pending = 0, rawFrame = 0; uint8_t queued = 0;
        const bool ownerReadable = owner && ReadTaskField(owner + 0x78, generation) &&
            ReadTaskField(owner + 0x7c, queued) && ReadTaskField(owner + 0x80, pending);
        const bool frameReadable = ReadTaskField(object + 0x1548, rawFrame);
        XS_LOG("TASK OBJECT this=%p owner=%p entry=%llu accepted=%llu rejected=%llu update=%llu after_increment=%llu generation=%u queued=%u pending=%#x raw_1548=%u owner_readable=%u frame_readable=%u",
            (void*)object, (void*)owner, (unsigned long long)record.entries.load(),
            (unsigned long long)record.accepted.load(), (unsigned long long)record.rejected.load(),
            (unsigned long long)record.updates.load(), (unsigned long long)record.finished.load(), generation, queued, pending, rawFrame, ownerReadable, frameReadable);
        WriterAudioState("fault", object);
    }
}
static void InstallWriterTaskTrace()
{
    char value[8]{};
    if (!GetEnvironmentVariableA("XSX_TASK_TRACE", value, sizeof value) || value[0] != '1') return;
    if (GetEnvironmentVariableA("XSX_TRAP", value, sizeof value) || GetEnvironmentVariableA("XSX_FREE_TRACE", value, sizeof value)) {
        XS_LOG("TASK trace disabled: use separately from XSX_TRAP/XSX_FREE_TRACE"); return;
    }
    using namespace xsx::diagnostic;
    const auto image = (uintptr_t)GetModuleHandleA(nullptr);
    const std::array<Point,4> points{{
        {image + 0x537f815,Instruction::RaxFromRcx},
        {image + 0x537f82b,Instruction::JnzPlusFive},
        {image + 0x6d8db6,Instruction::RsiFromRcx},
        {image + 0x6d912e,Instruction::RcxFromRsi},
    }};
    const HRESULT hr = g_writerTasks.Install(points, ObserveWriterTask);
    XS_LOG("TASK trace initialization hr=0x%08lX active=%u points=537f815,537f82b,6d8db6,6d912e", (unsigned long)hr, g_writerTasks.Active());
}
