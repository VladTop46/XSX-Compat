#include <windows.h>
#include <cstdio>

int main() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    printf("[XSX] EntryPoint reached\n");
    printf("[XSX] CPU threads: %lu\n",
           si.dwNumberOfProcessors);
    printf("[XSX] Page size: %lu\n",
           si.dwPageSize);

    HMODULE runtime = LoadLibraryW(
        L"xgameruntime.dll");

    if (!runtime) {
        printf("[XSX] GameRuntime unavailable: %lu\n",
               GetLastError());
        return 1;
    }

    printf("[XSX] GameRuntime loaded\n");

    FreeLibrary(runtime);
    return 0;
}
