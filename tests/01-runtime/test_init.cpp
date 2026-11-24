#include <windows.h>
#include <cstdio>

using InitFn = HRESULT (WINAPI*)();
using UninitFn = void (WINAPI*)();

int main()
{
    HMODULE mod = LoadLibraryW(
        L"xgameruntime.thunks.dll");

    if (!mod) {
        printf("[FAIL] LoadLibrary: %lu\n",
               GetLastError());
        return 1;
    }

    auto init = reinterpret_cast<InitFn>(
        GetProcAddress(mod, "XGameRuntimeInitialize"));

    auto uninit = reinterpret_cast<UninitFn>(
        GetProcAddress(mod, "XGameRuntimeUninitialize"));

    if (!init || !uninit)
        return 2;

    printf("[CALL] XGameRuntimeInitialize\n");
    fflush(stdout);

    HRESULT hr = init();

    printf("[RESULT] HRESULT = 0x%08lX\n",
           static_cast<unsigned long>(hr));

    if (SUCCEEDED(hr)) {
        printf("[PASS] Runtime initialized\n");

        uninit();

        printf("[PASS] Runtime uninitialized\n");
    } else {
        printf("[FAIL] Initialization failed\n");
    }

    FreeLibrary(mod);
    return FAILED(hr) ? 3 : 0;
}
