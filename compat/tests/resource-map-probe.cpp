// Real PC D3D12 backing + guest texture Map/Unmap. This checks CPU shadow
// identity only; writes are not uploaded to the GPU by this prototype.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>

static int checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #expr); } } while (0)
using Create = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using Commit = HRESULT (WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using Place = HRESULT (WINAPI*)(void*, void*, const D3D12_RESOURCE_DESC*, UINT,
    const D3D12_CLEAR_VALUE*, REFIID, void**);

int main()
{
    HMODULE module = LoadLibraryA("d3d12_xs.dll");
    Create create = nullptr;
    FARPROC symbol = module ? GetProcAddress(module, "D3D12XboxCreateDevice") : nullptr;
    static_assert(sizeof create == sizeof symbol);
    std::memcpy(&create, &symbol, sizeof create);
    CHECK(create);
    if (!create) return 1;
    ID3D12Device* device = nullptr;
    HRESULT hr = create(nullptr, nullptr, __uuidof(ID3D12Device), (void**)&device);
    CHECK(SUCCEEDED(hr) && device);
    if (!device) return 1;
    auto vt = *(void***)device;
    auto commit = (Commit)vt[76]; // Proven Xbox CreateCommittedResource contract.
    auto place = (Place)vt[133]; // Proven create-at-address contract.
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 16; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* texture = nullptr;
    hr = commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&texture);
    CHECK(SUCCEEDED(hr) && texture);
    if (texture) {
        void* cpu = nullptr;
        D3D12_RANGE range{0, 0};
        hr = texture->Map(0, &range, &cpu);
        CHECK(SUCCEEDED(hr) && cpu);
        if (cpu) {
            CHECK((UINT64)(uintptr_t)cpu == texture->GetGPUVirtualAddress());
            std::memset(cpu, 0x5a, 16);
            texture->Unmap(0, nullptr);
            void* again = nullptr;
            CHECK(SUCCEEDED(texture->Map(0, nullptr, &again)) && again == cpu);
            CHECK(again && ((unsigned char*)again)[15] == 0x5a);
            texture->Unmap(0, nullptr);
        }
        void* invalid = nullptr;
        CHECK(FAILED(texture->Map(1, nullptr, &invalid)));
        D3D12_RANGE invalidRange{2, 1};
        CHECK(texture->Map(0, &invalidRange, &invalid) == E_INVALIDARG && !invalid);
        texture->Release();
    }
    auto* title = (unsigned char*)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(title);
    ID3D12Resource* placed = nullptr;
    if (title) {
        hr = place(device, title, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            __uuidof(ID3D12Resource), (void**)&placed);
        CHECK(SUCCEEDED(hr) && placed);
        if (placed) {
            void* cpu = nullptr;
            CHECK(SUCCEEDED(placed->Map(0, nullptr, &cpu)) && cpu == title);
            if (cpu) ((unsigned char*)cpu)[31] = 0xa7;
            CHECK(title[31] == 0xa7);
            placed->Unmap(0, nullptr);
            placed->Release();
        }
        VirtualFree(title, 0, MEM_RELEASE);
    }
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = 1; desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    hr = commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload);
    CHECK(SUCCEEDED(hr) && upload);
    if (upload) {
        void* cpu = nullptr;
        CHECK(SUCCEEDED(upload->Map(0, nullptr, &cpu)) && cpu);
        if (cpu) std::memset(cpu, 0x3c, 256);
        upload->Unmap(0, nullptr);
        upload->Release();
    }
    device->Release();
    std::printf("[%s] resource Map checks=%d failures=%d scope=CPU-shadow-and-native-upload\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
