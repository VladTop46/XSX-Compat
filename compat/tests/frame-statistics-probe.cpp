// An unavailable statistics query must not advertise a partial record.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>

static int checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #expr); } } while (0)
using Create = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using Statistics = HRESULT (WINAPI*)(void*, UINT64, UINT, UINT*, void*);
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
    auto statistics = (Statistics)(*(void***)device)[81];
    unsigned char records[6 * 56 + 16];
    std::memset(records, 0xa5, sizeof records);
    UINT count = 6;
    CHECK(statistics(device, 0x123456789abcdef0, 0x11100, &count, records + 8) == S_FALSE);
    CHECK(count == 0);
    unsigned damaged = 0;
    for (auto byte : records) if (byte != 0xa5) ++damaged;
    CHECK(damaged == 0);
    CHECK(statistics(device, 1, 0x11100, nullptr, records) == E_POINTER);
    device->Release();
    std::printf("[%s] frame statistics checks=%d failures=%d records=0 output_damage=%u\n", failures ? "FAIL" : "PASS", checks, failures, damaged);
    return failures ? 1 : 0;
}
