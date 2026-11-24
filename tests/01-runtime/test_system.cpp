#include <windows.h>
#include <cstdio>
#include <cstdint>

int main()
{
    HMODULE mod = LoadLibraryW(L"xgameruntime.thunks.dll");
    if (!mod) return 1;

    auto init = reinterpret_cast<HRESULT(WINAPI*)()>(
        GetProcAddress(mod, "XGameRuntimeInitialize"));

    auto uninit = reinterpret_cast<void(WINAPI*)()>(
        GetProcAddress(mod, "XGameRuntimeUninitialize"));

    auto getDeviceType = reinterpret_cast<uint32_t(WINAPI*)()>(
        GetProcAddress(mod, "XSystemGetDeviceType"));

    if (!init || !uninit || !getDeviceType)
        return 2;

    HRESULT hr = init();

    printf("[INIT] 0x%08lX\n", (unsigned long)hr);

    if (FAILED(hr)) return 3;

    const char* names[] = {
        "Unknown",
        "PC",
        "Xbox One",
        "Xbox One S",
        "Xbox One X",
        "Xbox One X Devkit",
        "Xbox Series S",
        "Xbox Series X",
        "Scarlett Devkit"
    };

    uint32_t type = getDeviceType();

    printf("[DEVICE TYPE] 0x%08X\n", type);
    printf("[DEVICE NAME] %s\n",
           type < 9 ? names[type] : "Unrecognized");

    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);

    printf("[CPU THREADS] %lu\n",
           info.dwNumberOfProcessors);

    printf("[PAGE SIZE] %lu\n",
           info.dwPageSize);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);

    if (GlobalMemoryStatusEx(&mem)) {
        printf("[TOTAL RAM] %llu MiB\n",
               mem.ullTotalPhys / 1048576);

        printf("[AVAILABLE RAM] %llu MiB\n",
               mem.ullAvailPhys / 1048576);
    }

    uninit();
    FreeLibrary(mod);

    printf("[RESULT] PASS\n");
    return 0;
}
