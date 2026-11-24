#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdio>
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

#include <utility>
// ДИАГНОСТИЧЕСКАЯ ЗАГЛУШКА (подтверждено только то, что игра требует S_OK от AcpHalCreate, иначе assert в exe+0x43ad94e):
// объект с vtable из 32 слотов, каждый логирует вызов и возвращает S_OK. Реальный SHAPE command/message channel НЕ эмулируется.
template <int N> static HRESULT WINAPI ShapeStub(void* self, void* a, void* b, void* c) {
    static volatile long n = 0; if (InterlockedIncrement(&n) <= 24) std::fprintf(stderr, "[APU] SHAPE obj slot %d called (self=%p a=%p b=%p c=%p) -> S_OK (заглушка)\n", N, self, a, b, c);
    return S_OK;
}
template <int... I> static void FillShape(void** vt, std::integer_sequence<int, I...>) { ((vt[I] = (void*)&ShapeStub<I>), ...); }
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
    static void* vt[32]; static bool init = false;
    if (!init) { FillShape(vt, std::make_integer_sequence<int, 32>{}); init = true; }
    // obj указывает на ячейку, содержащую указатель на vtable: [obj] = vt
    static void* holder[2]; holder[0] = vt; holder[1] = nullptr; *output = holder;
    std::fprintf(stderr, "[APU] AcpHalCreate: возвращён объект-заглушка (S_OK), SHAPE channel не эмулируется\n");
    return S_OK;
}
}
