// GPU readback of the exact native presentation pass used by d3d12_xs.
// No game binaries, a window, or a network connection are needed.
#define INITGUID
#include "../d3d12_xs/surface_blit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using xsx::presentation::Ref;
using xsx::presentation::SurfaceBlit;
using xsx::presentation::Transition;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
static bool Good(HRESULT hr, const char* operation)
{
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) std::printf("[FAIL] operation=%s hr=0x%08lx\n", operation, (unsigned long)hr);
    return SUCCEEDED(hr);
}
static D3D12_RESOURCE_DESC Texture(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    return desc;
}
static bool Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType, Ref<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = heapType;
    return Good(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, __uuidof(ID3D12Resource), (void**)out.Put()), "buffer");
}
static std::array<unsigned char, 4> Pixel(UINT x, UINT y)
{
    return {(unsigned char)(13 + 29*x + 7*y), (unsigned char)(27 + 9*x + 31*y),
        (unsigned char)(61 + 17*x + 11*y), (unsigned char)(169 + 3*x + 5*y)};
}
static int Reference(UINT x, UINT y, UINT sw, UINT sh, UINT dw, UINT dh, unsigned component)
{
    const double u = std::clamp((x + 0.5) * sw / dw - 0.5, 0.0, double(sw - 1));
    const double v = std::clamp((y + 0.5) * sh / dh - 0.5, 0.0, double(sh - 1));
    const UINT x0 = (UINT)u, y0 = (UINT)v, x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
    const double a = u - x0, b = v - y0;
    const double top = Pixel(x0, y0)[component] * (1-a) + Pixel(x1, y0)[component] * a;
    const double bottom = Pixel(x0, y1)[component] * (1-a) + Pixel(x1, y1)[component] * a;
    return (int)std::lround(top * (1-b) + bottom * b);
}
struct Case { UINT sw, sh, dw, dh; DXGI_FORMAT format; D3D12_RESOURCE_STATES state; };
static bool Run(ID3D12Device* device, ID3D12CommandQueue* queue, SurfaceBlit& blit, const Case& test)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto sourceDesc = Texture(test.sw, test.sh, test.format);
    sourceDesc.Flags = test.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ?
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto targetDesc = Texture(test.dw, test.dh, DXGI_FORMAT_R8G8B8A8_UNORM);
    targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Ref<ID3D12Resource> source, target, upload, readback;
    if (!Good(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &sourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)source.Put()), "source") ||
        !Good(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &targetDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)target.Put()), "target")) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT srcFootprint{}, dstFootprint{};
    UINT64 srcBytes = 0, dstBytes = 0;
    device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &srcFootprint, nullptr, nullptr, &srcBytes);
    device->GetCopyableFootprints(&targetDesc, 0, 1, 0, &dstFootprint, nullptr, nullptr, &dstBytes);
    const UINT64 guardedBytes = dstBytes + 256;
    if (!Buffer(device, srcBytes, D3D12_HEAP_TYPE_UPLOAD, upload) ||
        !Buffer(device, guardedBytes, D3D12_HEAP_TYPE_READBACK, readback)) return false;
    unsigned char* data = nullptr;
    const D3D12_RANGE empty{0, 0};
    if (!Good(upload->Map(0, &empty, (void**)&data), "upload-map")) return false;
    std::memset(data, 0xcd, (size_t)srcBytes);
    for (UINT y = 0; y < test.sh; ++y) for (UINT x = 0; x < test.sw; ++x) {
        auto color = Pixel(x, y);
        if (test.format == DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(color[0], color[2]);
        std::memcpy(data + srcFootprint.Offset + y*srcFootprint.Footprint.RowPitch + x*4, color.data(), 4);
    }
    upload->Unmap(0, nullptr);
    if (!Good(readback->Map(0, &empty, (void**)&data), "guard-map")) return false;
    std::memset(data, 0xa7, (size_t)guardedBytes); readback->Unmap(0, nullptr);
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12DescriptorHeap> rtvs;
    Ref<ID3D12Fence> fence;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
    if (!Good(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)allocator.Put()), "allocator") ||
        !Good(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        __uuidof(ID3D12GraphicsCommandList), (void**)list.Put()), "list") ||
        !Good(device->CreateDescriptorHeap(&heapDesc, __uuidof(ID3D12DescriptorHeap), (void**)rtvs.Put()), "rtv-heap") ||
        !Good(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)fence.Put()), "fence")) return false;
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = srcFootprint;
    to.pResource = source.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Transition(list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, test.state);
    Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!Good(blit.Record(list.Get(), source.Get(), test.state, test.dw, test.dh, rtv), "blit-record")) return false;
    // The presenter must keep its source alive even when the caller drops it.
    auto* alias = source.Get(); alias->AddRef(); source.Reset();
    CHECK(alias->Release() >= 1);
    Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    from = {}; to = {};
    from.pResource = target.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = dstFootprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    if (!Good(list->Close(), "close")) { blit.ReleaseSource(); return false; }
    ID3D12CommandList* commands[]{list.Get()}; queue->ExecuteCommandLists(1, commands);
    // On a GPU failure stop the process without releasing in-flight objects.
    if (!Good(queue->Signal(fence.Get(), 1), "signal")) ExitProcess(1);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event);
    if (!event || !Good(fence->SetEventOnCompletion(1, event), "fence-event")) ExitProcess(1);
    const DWORD waited = WaitForSingleObject(event, 10000); CloseHandle(event);
    CHECK(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() == 1);
    if (waited != WAIT_OBJECT_0 || fence->GetCompletedValue() != 1) ExitProcess(1);
    CHECK(alias->GetDesc().Width == test.sw); // The pass still owns the reference.
    blit.ReleaseSource();
    const D3D12_RANGE range{0, (SIZE_T)guardedBytes};
    if (!Good(readback->Map(0, &range, (void**)&data), "readback-map")) return false;
    std::vector<bool> pixels((size_t)guardedBytes, false);
    unsigned errors = 0, guardErrors = 0, maxError = 0;
    for (UINT y = 0; y < test.dh; ++y) for (UINT x = 0; x < test.dw; ++x) for (unsigned c = 0; c < 4; ++c) {
        const size_t offset = (size_t)dstFootprint.Offset + y*dstFootprint.Footprint.RowPitch + x*4 + c;
        pixels[offset] = true;
        const unsigned difference = (unsigned)std::abs((int)data[offset] - Reference(x, y, test.sw, test.sh, test.dw, test.dh, c));
        maxError = std::max(maxError, difference);
        // Linear texture filtering has finite subtexel precision; allow two
        // UNORM8 levels, including the final render-target quantization.
        if (difference > 2) ++errors;
    }
    for (size_t i = 0; i < pixels.size(); ++i) if (!pixels[i] && data[i] != 0xa7) ++guardErrors;
    readback->Unmap(0, &empty);
    CHECK(errors == 0); CHECK(guardErrors == 0);
    std::printf("[BLIT] source=%ux%u target=%ux%u format=%u state=%#x pixels=%u errors=%u max_error=%u guard_errors=%u caller_reference_dropped=1\n",
        test.sw, test.sh, test.dw, test.dh, (unsigned)test.format, (unsigned)test.state,
        test.dw*test.dh, errors, maxError, guardErrors);
    return errors == 0 && guardErrors == 0;
}
int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE module = LoadLibraryW(L"d3d12.dll"); CHECK(module);
    auto create = xsx::presentation::Symbol<decltype(&D3D12CreateDevice)>(module, "D3D12CreateDevice");
    CHECK(create);
    if (!create) return 1;
    Ref<ID3D12Device> device;
    if (!Good(create(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)device.Put()), "device")) return 1;
    Ref<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    if (!Good(device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void**)queue.Put()), "queue")) return 1;
    SurfaceBlit blit;
    if (!Good(blit.Initialize(device.Get()), "blit-initialize")) return 1;
    CHECK(blit.Initialize(device.Get()) == S_OK);
    const Case cases[]{
        {8,4,8,4,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {8,4,4,2,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {4,3,13,9,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET},
        {8,4,5,3,DXGI_FORMAT_B8G8R8A8_UNORM,D3D12_RESOURCE_STATE_COMMON},
        {1,1,7,5,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {17,9,6,4,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    };
    unsigned completed = 0;
    for (const auto& test : cases) { if (!Run(device.Get(), queue.Get(), blit, test)) break; ++completed; }
    CHECK(completed == sizeof cases / sizeof cases[0]);
    std::printf("[%s] surface blit checks=%u failures=%u cases=%u scope=native-GPU-linear-sampling\n",
        failures ? "FAIL" : "PASS", checks, failures, completed);
    return failures ? 1 : 0;
}
