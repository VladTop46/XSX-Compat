#include <windows.h>
#include <cstdio>

int main()
{
    const wchar_t* dll =
        L"amd_fidelityfx_upscaler_xbox.dll";

    HMODULE mod = LoadLibraryW(dll);

    if (!mod) {
        printf("[FAIL] FidelityFX LoadLibrary: %lu\n",
               GetLastError());
        return 1;
    }

    printf("[PASS] Xbox FidelityFX loaded at %p\n", mod);

    FreeLibrary(mod);

    puts("[PASS] FidelityFX unloaded");
    return 0;
}
