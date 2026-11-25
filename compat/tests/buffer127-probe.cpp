// Exercise the installed Xbox slot127 thunk and real native buffer data flow.
// Buffer Map/VA keep PC semantics; this does not test console UMA coherence.
#define INITGUID
#include "../d3d12_xs/surface_blit.h"
#include <cstdint>

using xsx::presentation::Ref;
using xsx::presentation::Symbol;
static unsigned checks = 0, failures = 0, titleAllocations = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
static bool Good(HRESULT hr, const char* operation)
{
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) std::printf("[FAIL] operation=%s hr=0x%08lx\n", operation, (unsigned long)hr);
    return SUCCEEDED(hr);
}
static void* WINAPI Allocate(size_t, UINT64) { ++titleAllocations; return nullptr; }
static void WINAPI Free(void*, UINT64) { CHECK(false); }
extern "C" {
// A native buffer must not allocate an unused CPU shadow through these hooks.
__declspec(dllexport) void* __XMemAllocationHooks__[3]{(void*)&Allocate, (void*)&Free, nullptr};
}
using CreateDevice = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using Commit = HRESULT (WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, UINT, const DXGI_FORMAT*, REFIID, void**);
static D3D12_RESOURCE_DESC1 Description(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
    return desc;
}
static HRESULT Make(Commit commit, ID3D12Device* device, D3D12_HEAP_TYPE type,
    const D3D12_RESOURCE_DESC1& desc, Ref<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    return commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
        nullptr, nullptr, 0, nullptr, __uuidof(ID3D12Resource), (void**)out.Put());
}
static unsigned char Pattern(size_t offset, unsigned seed) { return (unsigned char)((offset*37 + seed*13 + 19) % 251); }
static bool Roundtrip(ID3D12Device* device, Commit commit, UINT bytes, unsigned seed)
{
    Ref<ID3D12Resource> upload, target, readback;
    const auto desc = Description(bytes, seed & 1 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    const auto guarded = Description(bytes + 256);
    if (!Good(Make(commit, device, D3D12_HEAP_TYPE_DEFAULT, desc, target), "default127") ||
        !Good(Make(commit, device, D3D12_HEAP_TYPE_UPLOAD, guarded, upload), "upload127") ||
        !Good(Make(commit, device, D3D12_HEAP_TYPE_READBACK, guarded, readback), "readback127")) return false;
    CHECK(target->GetDesc().Width == bytes && target->GetDesc().Flags == desc.Flags);
    CHECK(target->GetGPUVirtualAddress() && upload->GetGPUVirtualAddress() && readback->GetGPUVirtualAddress());
    D3D12_HEAP_PROPERTIES actual{}; D3D12_HEAP_FLAGS actualFlags{};
    CHECK(target->GetHeapProperties(&actual, &actualFlags) == S_OK && actual.Type == D3D12_HEAP_TYPE_DEFAULT);
    CHECK(upload->GetHeapProperties(&actual, &actualFlags) == S_OK && actual.Type == D3D12_HEAP_TYPE_UPLOAD);
    CHECK(readback->GetHeapProperties(&actual, &actualFlags) == S_OK && actual.Type == D3D12_HEAP_TYPE_READBACK);
    Ref<ID3D12Resource> identity;
    CHECK(target->QueryInterface(__uuidof(ID3D12Resource), (void**)identity.Put()) == S_OK && identity.Get() == target.Get());
    identity.Reset();
    CHECK(target->AddRef() >= 2); CHECK(target->Release() >= 1);
    unsigned char* data = nullptr;
    const D3D12_RANGE empty{0,0};
    if (!Good(upload->Map(0, &empty, (void**)&data), "upload-map") || !data) return false;
    for (UINT i = 0; i < bytes + 256; ++i) data[i] = Pattern(i, seed);
    void* repeated = nullptr;
    CHECK(upload->Map(0, &empty, &repeated) == S_OK && repeated == data);
    const D3D12_RANGE written{0,bytes + 256};
    upload->Unmap(0, &written); upload->Unmap(0, &written);
    if (!Good(readback->Map(0, &empty, (void**)&data), "readback-guard-map") || !data) return false;
    std::memset(data, 0xa7, bytes + 256); readback->Unmap(0, &written);
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    if (!Good(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)queue.Put()), "queue") ||
        !Good(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)allocator.Put()), "allocator") ||
        !Good(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            __uuidof(ID3D12GraphicsCommandList), (void**)list.Put()), "list") ||
        !Good(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)fence.Put()), "fence")) return false;
    // Buffer UNDEFINED layout starts in COMMON. The first copy promotes it;
    // the explicit barrier orders the write before the second copy reads it.
    list->CopyBufferRegion(target.Get(), 0, upload.Get(), 0, bytes);
    xsx::presentation::Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, bytes);
    xsx::presentation::Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (!Good(list->Close(), "close")) return false;
    ID3D12CommandList* commands[]{list.Get()}; queue->ExecuteCommandLists(1, commands);
    if (!Good(queue->Signal(fence.Get(), 1), "signal")) ExitProcess(1);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); CHECK(event);
    if (!event || !Good(fence->SetEventOnCompletion(1, event), "fence-event")) ExitProcess(1);
    const DWORD waited = WaitForSingleObject(event, 10000); CloseHandle(event);
    CHECK(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() == 1);
    if (waited != WAIT_OBJECT_0 || fence->GetCompletedValue() != 1) ExitProcess(1);
    const D3D12_RANGE range{0,bytes + 256};
    if (!Good(readback->Map(0, &range, (void**)&data), "readback-map") || !data) return false;
    unsigned errors = 0, guardErrors = 0;
    for (UINT i = 0; i < bytes; ++i) if (data[i] != Pattern(i, seed)) ++errors;
    for (UINT i = bytes; i < bytes + 256; ++i) if (data[i] != 0xa7) ++guardErrors;
    readback->Unmap(0, &empty);
    CHECK(errors == 0); CHECK(guardErrors == 0); CHECK(titleAllocations == 0);
    // Submit resources survive until the real fence, then all wrapper refs
    // must pair with native final release. No title allocator owns these.
    list.Reset();
    auto finalRelease = [](Ref<ID3D12Resource>& resource) {
        auto* native = resource.Get(); native->AddRef(); resource.Reset(); CHECK(native->Release() == 0);
    };
    finalRelease(target); finalRelease(upload); finalRelease(readback);
    std::printf("[GPU] buffer127 bytes=%u flags=%#x errors=%u guard_errors=%u title_allocations=%u\n",
        bytes, (unsigned)desc.Flags, errors, guardErrors, titleAllocations);
    return errors == 0 && guardErrors == 0;
}
int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetEnvironmentVariableA("XSX_TRACE_RETURNS", "1");
    HMODULE module = LoadLibraryW(L"d3d12_xs.dll"); CHECK(module);
    auto create = Symbol<CreateDevice>(module, "D3D12XboxCreateDevice"); CHECK(create);
    if (!create) return 1;
    Ref<ID3D12Device> device;
    if (!Good(create(nullptr, nullptr, __uuidof(ID3D12Device), (void**)device.Put()), "device")) return 1;
    const auto commit = (Commit)(*(void***)device.Get())[127];
    const UINT sizes[]{768,768,768,768,115200,98304,245760};
    unsigned completed = 0;
    for (UINT bytes : sizes) {
        if (!Roundtrip(device.Get(), commit, bytes, completed + 1)) break;
        ++completed;
    }
    CHECK(completed == sizeof sizes / sizeof sizes[0]);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto desc = Description(768);
    CHECK(commit(device.Get(), &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
        nullptr, nullptr, 0, nullptr, __uuidof(ID3D12Resource), nullptr) == S_FALSE);
    constexpr uintptr_t sentinel = 0x12345678aabbccddull;
    struct { UINT64 before = sentinel; void* value = (void*)sentinel; UINT64 after = sentinel; } output;
    auto invoke = [&](const D3D12_RESOURCE_DESC1& description, D3D12_BARRIER_LAYOUT layout, REFIID iid) {
        output.value = (void*)sentinel;
        const HRESULT hr = commit(device.Get(), &heap, D3D12_HEAP_FLAG_NONE, &description, layout,
            nullptr, nullptr, 0, nullptr, iid, &output.value);
        CHECK(!output.value && output.before == sentinel && output.after == sentinel);
        // A backend may unexpectedly accept an input this probe rejects.
        // Report that failure, but do not leak its returned resource.
        if (output.value && output.value != (void*)sentinel) {
            ((IUnknown*)output.value)->Release(); output.value = nullptr;
        }
        return hr;
    };
    CHECK(invoke(desc, D3D12_BARRIER_LAYOUT_COMMON, __uuidof(ID3D12Resource)) == E_INVALIDARG);
    CHECK(invoke(desc, D3D12_BARRIER_LAYOUT_UNDEFINED, __uuidof(ID3D12Fence)) == E_NOINTERFACE);
    desc.Width = 0;
    CHECK(invoke(desc, D3D12_BARRIER_LAYOUT_UNDEFINED, __uuidof(ID3D12Resource)) == E_INVALIDARG);
    CHECK(commit(device.Get(), &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
        nullptr, nullptr, 0, nullptr, __uuidof(ID3D12Resource), nullptr) == E_INVALIDARG);
    desc.Width = 768; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(FAILED(invoke(desc, D3D12_BARRIER_LAYOUT_UNDEFINED, __uuidof(ID3D12Resource))));
    CHECK(titleAllocations == 0);
    std::printf("[%s] buffer127 checks=%u failures=%u cases=%u scope=native-buffers-no-UMA-bridge\n",
        failures ? "FAIL" : "PASS", checks, failures, completed);
    return failures ? 1 : 0;
}
