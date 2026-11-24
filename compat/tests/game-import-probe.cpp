// Resolve the reported game's direct imports without loading its executable.
// Normal DLL initializers run; imported game functions are never invoked.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "game-imports.generated.h"

static std::string Utf8(const wchar_t* value)
{
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (!count) return "<path conversion failed>";
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const wchar_t* dllDirectory = nullptr;
    const wchar_t* selected = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--help")) {
            puts("Usage: game-import-probe.exe [--dll-dir DIRECTORY] [--module NAME.dll]");
            return 0;
        }
        if (!wcscmp(argv[i], L"--dll-dir") && i + 1 < argc && !dllDirectory)
            dllDirectory = argv[++i];
        else if (!wcscmp(argv[i], L"--module") && i + 1 < argc && !selected)
            selected = argv[++i];
        else {
            puts("[GAME PROBE] invalid or duplicate option; use --help");
            return 2;
        }
    }
    // A single explicit fixture directory, logged below; no files are copied.
    if (dllDirectory) {
        wchar_t absolute[32768]{};
        const DWORD count = GetFullPathNameW(dllDirectory, 32768, absolute, nullptr);
        if (!count || count >= 32768 || !SetDllDirectoryW(absolute)) {
            printf("[GAME PROBE] dll-dir setup failed error=%lu\n", GetLastError());
            return 3;
        }
        printf("[GAME PROBE] dll-dir=%s\n", Utf8(absolute).c_str());
    }
    printf("[GAME PROBE] target=%s report_sha256=%s\n", GameTarget::image,
           GameTarget::report_sha256);
    puts("[GAME PROBE] DLL initializers run; imported game functions are not called.");
    const auto selectedName = selected ? Utf8(selected) : std::string();
    unsigned modules = 0, required = 0, resolved = 0, failedLoads = 0;
    unsigned missingExports = 0, unloadedImports = 0;
    std::vector<HMODULE> loaded;
    for (const auto& module : GameTarget::modules) {
        if (selected && _stricmp(module.name, selectedName.c_str())) continue;
        ++modules;
        required += module.count;
        // Flush-before-load output identifies a DLL whose initializer crashes.
        printf("[GAME LOAD] %s required=%u\n", module.name, module.count);
        const HMODULE handle = LoadLibraryA(module.name);
        if (!handle) {
            printf("[GAME DLL FAIL] %s error=%lu\n", module.name, GetLastError());
            ++failedLoads;
            unloadedImports += module.count;
            continue;
        }
        loaded.push_back(handle);
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(handle, path, 32768);
        printf("[GAME DLL] %s path=%s\n", module.name,
               length && length < 32768 ? Utf8(path).c_str() : "<unavailable>");
        unsigned found = 0;
        for (unsigned i = 0; i < module.count; ++i) {
            const auto& symbol = module.imports[i];
            const auto address = GetProcAddress(handle, symbol.name ? symbol.name :
                MAKEINTRESOURCEA(symbol.ordinal));
            if (address) {
                ++found;
                ++resolved;
            } else {
                ++missingExports;
                if (symbol.name)
                    printf("[GAME EXPORT FAIL] %s!%s error=%lu\n",
                           module.name, symbol.name, GetLastError());
                else
                    printf("[GAME EXPORT FAIL] %s!#%u error=%lu\n",
                           module.name, symbol.ordinal, GetLastError());
            }
        }
        printf("[GAME RESOLVE] %s found=%u/%u\n", module.name, found, module.count);
    }
    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) FreeLibrary(*it);
    if (!modules) {
        puts("[GAME PROBE] module not found in report");
        return 2;
    }
    const int exitCode = failedLoads || missingExports ? 8 : 0;
    printf("[GAME PROBE] exit=%d modules=%u imports=%u resolved=%u failed_loads=%u "
           "unresolved_exports=%u imports_in_unloaded_dlls=%u ABI=unverified\n",
           exitCode, modules, required, resolved, failedLoads, missingExports, unloadedImports);
    return exitCode;
}
