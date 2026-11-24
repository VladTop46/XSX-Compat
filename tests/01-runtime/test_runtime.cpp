#include <windows.h>
#include <cstdio>

static bool test(
    const wchar_t* name,
    const char* const* exports,
    size_t count)
{
    std::printf("\n[LOAD] %ls\n", name);
    std::fflush(stdout);

    HMODULE mod = LoadLibraryW(name);

    if (!mod) {
        std::printf("[FAIL] GetLastError = %lu\n",
            GetLastError());
        return false;
    }

    std::printf("[PASS] Base = %p\n", (void*)mod);

    bool ok = true;

    for (size_t i = 0; i < count; ++i) {
        FARPROC ptr = GetProcAddress(mod, exports[i]);

        std::printf("[%s] %-32s %p\n",
            ptr ? "PASS" : "FAIL",
            exports[i], (void*)ptr);

        if (!ptr)
            ok = false;
    }

    FreeLibrary(mod);
    std::printf("[UNLOAD] OK\n");

    return ok;
}

int main()
{
    const char* runtime[] = {
        "DllCanUnloadNow",
        "InitializeApiImpl",
        "InitializeApiImplEx",
        "InitializeApiImplEx2",
        "QueryApiImpl",
        "UninitializeApiImpl",
        "XErrorReport"
    };

    const char* thunks[] = {
        "XGameRuntimeInitialize",
        "XGameRuntimeUninitialize",
        "XTaskQueueCreate",
        "XTaskQueueDispatch",
        "XTaskQueueSubmitCallback",
        "XTaskQueueCloseHandle",
        "XSystemGetDeviceType"
    };

    bool a = test(L"xgameruntime.dll",
        runtime, sizeof(runtime)/sizeof(runtime[0]));

    bool b = test(L"xgameruntime.thunks.dll",
        thunks, sizeof(thunks)/sizeof(thunks[0]));

    return a && b ? 0 : 1;
}
