// Exercises the installed device78/79/80 ABI through the actual shim DLL.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
using Create = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using SetInterval = HRESULT (WINAPI*)(void*, void*, UINT, UINT, UINT);
using Schedule = HRESULT (WINAPI*)(void*, UINT, UINT, const void*, UINT);
using Wait = HRESULT (WINAPI*)(void*, UINT, UINT, const void*, UINT, UINT64*);
int main()
{
    HMODULE module = LoadLibraryA("d3d12_xs.dll");
    auto symbol = module ? GetProcAddress(module, "D3D12XboxCreateDevice") : nullptr;
    Create create = nullptr;
    static_assert(sizeof create == sizeof symbol);
    std::memcpy(&create, &symbol, sizeof create);
    CHECK(create);
    if (!create) return 1;
    ID3D12Device* device = nullptr;
    CHECK(SUCCEEDED(create(nullptr, nullptr, __uuidof(ID3D12Device), (void**)&device)) && device);
    if (!device) return 1;
    auto* vt = *(void***)device;
    const auto interval = (SetInterval)vt[78];
    const auto schedule = (Schedule)vt[79];
    const auto wait = (Wait)vt[80];
    CHECK(interval(device, nullptr, 10000, 3, 0) == S_OK);
    CHECK(schedule(device, 0, 0, nullptr, 0) == S_OK);
    CHECK(schedule(device, 2, 0, nullptr, 0) == S_OK);
    struct Guard { UINT64 before, token, after; } value{0x123456789abcdef0, 0xfedcba9876543210, 0x123456789abcdef0};
    CHECK(wait(device, 0, 0, nullptr, 0, &value.token) == S_FALSE);
    CHECK(value.token == 0xfedcba9876543210);
    CHECK(wait(device, 0, 0, nullptr, 2, &value.token) == S_FALSE);
    CHECK(value.token == 1);
    value.token = 0xfedcba9876543210;
    CHECK(wait(device, 2, INFINITE, nullptr, 0, &value.token) == S_OK);
    CHECK(value.token == 0xfedcba9876543210); // Full 64-bit token preserved.
    CHECK(wait(device, 0, 0, nullptr, 2, &value.token) == S_FALSE);
    CHECK(value.token == 2); // Flip did not consume token2.
    const ULONGLONG begin = GetTickCount64();
    for (UINT64 expected = 3; expected <= 6; ++expected) {
        CHECK(wait(device, 2, INFINITE, nullptr, 0, &value.token) == S_OK);
        CHECK(value.token == expected - 1);
        CHECK(wait(device, 0, 0, nullptr, 2, &value.token) == S_FALSE);
        CHECK(value.token == expected);
    }
    const ULONGLONG elapsed = GetTickCount64() - begin;
    CHECK(elapsed >= 20 && elapsed < 5000); // Wide wall-clock bound; exact tests are deterministic.
    const UINT64 preserved = value.token;
    CHECK(wait(device, 0, 0, nullptr, 0, &value.token) == S_FALSE);
    CHECK(value.token == preserved);
    CHECK(wait(device, 3, 0, nullptr, 0, &value.token) == E_INVALIDARG);
    CHECK(wait(device, 0, 0, nullptr, 1, &value.token) == E_NOTIMPL);
    CHECK(schedule(device, 0, 1, nullptr, 0) == E_NOTIMPL);
    CHECK(interval(device, nullptr, 10000, 3, 1) == E_NOTIMPL); // Stack flags argument.
    CHECK(value.token == preserved && value.before == 0x123456789abcdef0 && value.after == value.before);
    device->Release();
    std::printf("[FRAME] interval_us=10000 period=3 flips=5 tokens=6 elapsed_ms=%llu sentinel_preserved=1\n", elapsed);
    std::printf("[%s] frame events checks=%d failures=%d\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
