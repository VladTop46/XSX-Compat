// Xbox ResetX139 must actually reopen a PC list, including its allocator.
// Reuse the list and verify the recorded copy through GPU readback.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>

static int checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #expr); } } while (0)
using Create = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using CreateList = HRESULT (WINAPI*)(void*, const UINT*, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using ResetX = HRESULT (WINAPI*)(void*, ID3D12CommandAllocator*, ID3D12PipelineState*, UINT64);
using Commit = HRESULT (WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
#ifdef XSX_WRITE_PROBE
#include "list-write-probe.h"
#endif

int main()
{
    HMODULE module = LoadLibraryA("d3d12_xs.dll");
    FARPROC symbol = module ? GetProcAddress(module, "D3D12XboxCreateDevice") : nullptr;
    Create create = nullptr;
    static_assert(sizeof create == sizeof symbol);
    std::memcpy(&create, &symbol, sizeof create);
    CHECK(create);
    if (!create) return 1;
    ID3D12Device* device = nullptr;
    CHECK(SUCCEEDED(create(nullptr, nullptr, __uuidof(ID3D12Device), (void**)&device)) && device);
    if (!device) return 1;
    auto vt = *(void***)device;
    auto createList = (CreateList)vt[73];
    auto commit = (Commit)vt[76];
    ID3D12CommandAllocator *allocator = nullptr, *other = nullptr;
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&other))));
    if (!allocator || !other) return 1;
    UINT xboxDesc[8]{};
    ID3D12GraphicsCommandList* list = nullptr;
    CHECK(SUCCEEDED(createList(device, xboxDesc, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&list)) && list);
    if (!list) return 1;
    auto reset = (ResetX)(*(void***)list)[139];
    CHECK(SUCCEEDED(list->Close()));
    // Two Close calls without an actual Reset reproduce the old E_FAIL.
    CHECK(FAILED(list->Close()));
    for (int i = 0; i < 4; ++i) {
        auto* current = (i & 1) ? other : allocator;
        CHECK(SUCCEEDED(current->Reset()));
        CHECK(SUCCEEDED(reset(list, current, nullptr, 0)));
        CHECK(SUCCEEDED(list->Close()));
    }
    CHECK(reset(list, allocator, nullptr, 1) == E_NOTIMPL);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ID3D12CommandQueue* queue = nullptr;
    CHECK(SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && queue);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 16 * sizeof(UINT); desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource *upload = nullptr, *readback = nullptr;
    CHECK(SUCCEEDED(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload)) && upload);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    CHECK(SUCCEEDED(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&readback)) && readback);
    if (!queue || !upload || !readback) return 1;
    constexpr UINT sentinel = 0xcafebabe;
    const UINT expected[4]{0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f001};
    UINT* memory = nullptr;
    CHECK(SUCCEEDED(upload->Map(0, nullptr, (void**)&memory)) && memory);
    if (!memory) return 1;
    std::memcpy(memory, expected, sizeof expected);
    upload->Unmap(0, nullptr);
    memory = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, (void**)&memory)) && memory);
    if (!memory) return 1;
    for (int i = 0; i < 16; ++i) memory[i] = sentinel;
    readback->Unmap(0, nullptr);
    CHECK(SUCCEEDED(allocator->Reset()));
    CHECK(SUCCEEDED(reset(list, allocator, nullptr, 0)));
    list->CopyBufferRegion(readback, 0, upload, 0, sizeof expected);
#ifdef XSX_WRITE_PROBE
    ListWriteProbe writes{};
    writes.Record(list);
#endif
    CHECK(SUCCEEDED(list->Close()));
    ID3D12CommandList* submitted[]{list};
    queue->ExecuteCommandLists(1, submitted);
#ifdef XSX_WRITE_PROBE
    writes.Verify(queue);
    const UINT64 previousTimestamp = writes.words[3];
    writes.Fill(); QueryPerformanceCounter(&writes.before);
    CHECK(SUCCEEDED(writes.ExecuteX(queue, list)));
    writes.Verify(queue);
    CHECK(writes.words[3] >= previousTimestamp);
#endif
    ID3D12Fence* fence = nullptr;
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence);
    if (!fence) return 1;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(done);
    CHECK(SUCCEEDED(fence->SetEventOnCompletion(1, done)));
    CHECK(SUCCEEDED(queue->Signal(fence, 1)));
    DWORD wait = WaitForSingleObject(done, 5000);
    CHECK(wait == WAIT_OBJECT_0);
    if (wait == WAIT_OBJECT_0) {
        memory = nullptr;
        CHECK(SUCCEEDED(readback->Map(0, nullptr, (void**)&memory)) && memory);
        if (memory) {
            unsigned errors = 0;
            for (int i = 0; i < 16; ++i)
                if (memory[i] != (i < 4 ? expected[i] : sentinel)) ++errors;
            CHECK(errors == 0);
            std::printf("[GPU] reset139 copied=4 guard=12 errors=%u\n", errors);
            readback->Unmap(0, nullptr);
        }
        // Keep the original PC Reset slot working after Xbox ResetX.
        CHECK(SUCCEEDED(other->Reset()));
        CHECK(SUCCEEDED(list->Reset(other, nullptr)));
        CHECK(SUCCEEDED(list->Close()));
#ifdef XSX_WRITE_PROBE
        writes.Fill();
        CHECK(SUCCEEDED(writes.ExecuteX(queue, list)));
        CHECK(SUCCEEDED(fence->SetEventOnCompletion(2, done)));
        CHECK(SUCCEEDED(queue->Signal(fence, 2)));
        CHECK(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
        unsigned stale = 0;
        for (auto word : writes.words) if (word != ListWriteProbe::guard) ++stale;
        CHECK(stale == 0);
        std::printf("[WRITES] phases=record,replay,reset clock=CPU-scaled reset_damage=%u\n", stale);
#endif
    }
    if (done) CloseHandle(done);
    fence->Release(); readback->Release(); upload->Release();
    list->Release(); other->Release(); allocator->Release(); queue->Release(); device->Release();
#ifdef XSX_WRITE_PROBE
    const char* scope = "writes";
#else
    const char* scope = "ResetX";
#endif
    std::printf("[%s] list %s checks=%d failures=%d\n", failures ? "FAIL" : "PASS", scope, checks, failures);
    return failures ? 1 : 0;
}
