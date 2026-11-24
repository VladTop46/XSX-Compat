#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "apu_memory.h"

namespace {
xsx::audio::Memory memory;
std::mutex shape_mutex;
void* shape_xma = nullptr;
struct ShapeContexts {
    uint32_t counts[5];
    uint32_t reserved;
    void* arrays[5];
};
static_assert(offsetof(ShapeContexts, arrays) == 24 && sizeof(ShapeContexts) == 64);
}

#include <deque>
#include <mutex>
// ---- Объект AcpHal (команды SHAPE / сообщения) --------------------------------------------------------------------------
// Раскладка и семантика — по открытой реализации VIctory (crates/victory-hle/src/modules/audio.rs): vtable (без QueryInterface):
//   [0] HRESULT connect(this, u32 commands, u32 messages)         — commands != 0, messages = ёмкость очереди сообщений
//   [1] HRESULT disconnect(this)                                    — E_FAIL, если не подключён; кладёт DISCONNECTED
//   [2] HRESULT submit(this, u32 command, u64 id, u32 frame, const void* data, u32 notification)
//   [3] bool    pop_message(this, Message* out)                     — FIFO, не блокирует
//   [4] u32     message_count(this)
//   [5] void    release(this)
// Команды выполняются мгновенно: REGISTER_MESSAGES(word0 маска kind) / UNREGISTER_MESSAGES; LOAD_FLOWGRAPH(word1 = id) → сообщение
// FLOWGRAPH_COMPLETED; команда с id != 0 → COMMAND_COMPLETED{data[0..4]=command, [4..12]=id, [12..16]=frame}; notification (адрес APU-кучи) ← 1.
// Остальные SHAPE-команды (контексты, XMA) принимаются и НЕ исполняются. fmod ждёт подтверждения первой команды в System_Create.
namespace acp {
enum : uint32_t { LOAD_FLOWGRAPH = 1, REGISTER_MESSAGES = 2, UNREGISTER_MESSAGES = 3 };
enum : uint32_t { FLOWGRAPH_COMPLETED = 0x02, COMMAND_COMPLETED = 0x10, DISCONNECTED = 0x80 };
#pragma pack(push, 1)
struct Message { uint32_t kind, dropped, usec; uint8_t data[16]; };
#pragma pack(pop)
static_assert(sizeof(Message) == 28);
struct Acp {
    void** vt; std::mutex m; bool connected = false; uint32_t mask = 0; std::deque<Message> q; size_t capacity = 0; uint32_t dropped = 0; DWORD t0 = GetTickCount();
    uint32_t usec() const { return (GetTickCount() - t0) * 1000u; }
    uint32_t frame() const { return (uint32_t)((uint64_t)usec() * 375 / 1000000); }
    void post(uint32_t kind, const void* d, size_t n) {
        if (!(mask & kind)) return;
        if (q.size() >= capacity) { dropped++; return; }
        Message msg{}; msg.kind = kind; msg.dropped = dropped; dropped = 0; msg.usec = usec(); if (d && n <= 16) memcpy(msg.data, d, n); q.push_back(msg);
    }
};
static HRESULT WINAPI Connect(Acp* a, uint32_t commands, uint32_t messages) {
    if (!commands) return E_INVALIDARG;
    std::lock_guard<std::mutex> l(a->m); a->connected = true; a->capacity = messages;
    std::fprintf(stderr, "[APU] Acp::connect(commands=%u, messages=%u) -> S_OK\n", commands, messages); return S_OK;
}
static HRESULT WINAPI Disconnect(Acp* a) { std::lock_guard<std::mutex> l(a->m); if (!a->connected) return E_FAIL; a->post(DISCONNECTED, nullptr, 0); a->connected = false; return S_OK; }
static HRESULT WINAPI Submit(Acp* a, uint32_t command, uint64_t id, uint32_t frame, const void* data, uint32_t notification) {
    std::lock_guard<std::mutex> l(a->m);
    auto word = [&](size_t i) -> uint32_t { return data && !IsBadReadPtr((const uint32_t*)data + i, 4) ? ((const uint32_t*)data)[i] : 0; };
    static volatile long seen[64]; if (command < 64 && !InterlockedExchange(&seen[command], 1)) std::fprintf(stderr, "[APU] Acp::submit command=%u id=%#llx frame=%u data=%p notification=%#x (первый раз)\n", command, (unsigned long long)id, frame, data, notification);
    switch (command) {
    case REGISTER_MESSAGES: a->mask |= word(0); break;
    case UNREGISTER_MESSAGES: a->mask &= ~word(0); break;
    case LOAD_FLOWGRAPH: { uint32_t fg = word(1); a->post(FLOWGRAPH_COMPLETED, &fg, 4); break; }
    default: break;
    }
    if (id) { uint8_t done[16] = {}; memcpy(done, &command, 4); memcpy(done + 4, &id, 8); uint32_t f = a->frame(); memcpy(done + 12, &f, 4); a->post(COMMAND_COMPLETED, done, 16); }
    if (notification) { void* p = memory.pointer(notification); if (p) *(volatile uint32_t*)p = 1; }
    return S_OK;
}
static bool WINAPI PopMessage(Acp* a, Message* out) { std::lock_guard<std::mutex> l(a->m); if (a->q.empty()) return false; if (out && !IsBadWritePtr(out, sizeof *out)) *out = a->q.front(); a->q.pop_front(); return true; }
static uint32_t WINAPI MessageCount(Acp* a) { std::lock_guard<std::mutex> l(a->m); return (uint32_t)a->q.size(); }
static void WINAPI Release(Acp* a) { delete a; }
static void* g_vt[6] = { (void*)Connect, (void*)Disconnect, (void*)Submit, (void*)PopMessage, (void*)MessageCount, (void*)Release };
}
extern "C" {
// Two DWORD arguments observed at exe RVA 0x43ad933. Pool policy is opaque;
// record the request, but do not invent a cached/uncached heap partition.
HRESULT WINAPI ApuCreateHeap(uint32_t first, uint32_t second) {
    std::fprintf(stderr, "[APU] CreateHeap first=0x%x second=0x%x (CPU address-space backend)\n", first, second);
    return S_OK;
}

// exe RVA 0x43a6110: RCX=&va, RDX=null, R8=size, R9=0x800,
// stack argument 5=caller mode. SDK samples corroborate the output-pointer ABI.
HRESULT WINAPI ApuAlloc(void** cpu, uint32_t* apu, uint32_t size, uint32_t alignment, uint32_t mode) {
    const HRESULT hr = memory.allocate(cpu, apu, size, alignment);
    std::fprintf(stderr, "[APU] Alloc size=%u alignment=%u mode=0x%x hr=0x%08lx CPU=%p\n",
                 size, alignment, mode, static_cast<unsigned long>(hr), cpu ? *cpu : nullptr);
    return hr;
}

HRESULT WINAPI ApuFree(void* cpu) { return memory.release(cpu); }

BOOL WINAPI ApuIsVirtualAddressValid(const void* cpu, uint32_t size) {
    return memory.address(cpu, size) != 0;
}

// This game's newer dialect is ONE input pointer -> EAX bus address.
// In particular it is NOT the older (&apu, cpu, size) -> HRESULT signature.
uint32_t WINAPI ApuMapVirtualAddress(const void* cpu) { return memory.address(cpu); }
void* WINAPI ApuMapApuAddress(uint32_t apu) { return memory.pointer(apu); }

HRESULT WINAPI AcpHalAllocateShapeContexts(ShapeContexts* contexts) {
    if (!contexts) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(shape_mutex);
    if (shape_xma) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    for (auto& p : contexts->arrays) p = nullptr;
    // Only the observed fifth array is established: 350 entries, 64 bytes each.
    for (int i = 0; i < 4; ++i) if (contexts->counts[i]) return E_NOTIMPL;
    if (contexts->counts[4] > UINT32_MAX / 64) return E_OUTOFMEMORY;
    if (!contexts->counts[4]) return S_OK;
    const HRESULT hr = memory.allocate(&shape_xma, nullptr, contexts->counts[4] * 64, 256);
    if (SUCCEEDED(hr)) contexts->arrays[4] = shape_xma;
    return hr;
}

// Caller at RVA 0x43ae899 prepares NO parameters: process-wide release.
HRESULT WINAPI AcpHalReleaseShapeContexts() {
    std::lock_guard<std::mutex> lock(shape_mutex);
    const HRESULT hr = memory.release(shape_xma);
    shape_xma = nullptr;
    return hr;
}

HRESULT WINAPI AcpHalCreate(void** output) {
    if (!output) return E_INVALIDARG;
    auto* a = new acp::Acp(); a->vt = acp::g_vt; *output = a;
    std::fprintf(stderr, "[APU] AcpHalCreate -> объект Acp %p (команды мгновенные, сообщения по маске)\n", (void*)a);
    return S_OK;
}
}
