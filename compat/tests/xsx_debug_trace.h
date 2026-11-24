#pragma once
// Capture only this Xbox DLL's named OutputDebugStringA import. Disk DLLs
// and native VKD3D objects stay untouched; restore before unloading the DLL.
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace XsxDebugTrace {
using OutputDebugFn = void (WINAPI*)(LPCSTR);
inline OutputDebugFn original = nullptr;
inline void** slot = nullptr;

inline void WINAPI Forward(LPCSTR message) {
    std::printf("[XBOX DEBUG] %s\n", message ? message : "<null>");
    original(message);
}

inline void Install(HMODULE module) {
    if (!module || slot) return;
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return;
    const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        if (!imports->OriginalFirstThunk) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + imports->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* imported = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(imported->Name),
                            "OutputDebugStringA")) continue;
            auto* target = reinterpret_cast<void**>(&addresses->u1.Function);
            DWORD protection = 0;
            if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &protection)) {
                std::printf("[XBOX DEBUG] IAT trace install failed: %lu\n", GetLastError());
                return;
            }
            original = reinterpret_cast<OutputDebugFn>(addresses->u1.Function);
            InterlockedExchangePointer(target, reinterpret_cast<void*>(&Forward));
            slot = target;
            DWORD unused = 0;
            if (!VirtualProtect(target, sizeof(void*), protection, &unused))
                std::printf("[XBOX DEBUG] IAT protection restore failed: %lu\n", GetLastError());
            std::puts("[ABI] Xbox OutputDebugStringA trace enabled");
            return;
        }
    }
}

inline void Restore() {
    if (!slot) return;
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
        std::printf("[XBOX DEBUG] IAT trace restore failed: %lu\n", GetLastError());
        return;
    }
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(original));
    DWORD unused = 0;
    if (!VirtualProtect(slot, sizeof(void*), protection, &unused))
        std::printf("[XBOX DEBUG] IAT protection restore failed: %lu\n", GetLastError());
    slot = nullptr;
}
} // namespace XsxDebugTrace
