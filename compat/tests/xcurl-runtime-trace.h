#pragma once
// Diagnostic only: trace this test DLL's runtime binding, without modifying
// original files, native object vtables or any other loaded module's imports.
namespace xcurl_trace {
using Initialize = HRESULT(WINAPI*)(uint64_t, uint64_t, uint64_t, void*);
using Query = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
inline Initialize initialize;
inline Query query;

inline HRESULT WINAPI trace_initialize(uint64_t first, uint64_t second, uint64_t flags, void* options) {
    HRESULT result = initialize(first, second, flags, options);
    std::printf("[XCURL TRACE] InitializeApiImplEx2 flags=0x%llx hr=0x%08lx\n",
                static_cast<unsigned long long>(flags), static_cast<unsigned long>(result));
    return result;
}
inline HRESULT WINAPI trace_query(const GUID* cls, const GUID* iid, void** output) {
    HRESULT result = query(cls, iid, output);
    std::printf("[XCURL TRACE] QueryApiImpl class=%08lx iid=%08lx hr=0x%08lx\n",
                cls ? cls->Data1 : 0ul, iid ? iid->Data1 : 0ul, static_cast<unsigned long>(result));
    if (result == static_cast<HRESULT>(0x8924010d) && cls && iid && output &&
        xsx::network::equal(*cls, xsx::network::service_id) &&
        xsx::network::equal(*iid, xsx::network::interface_id)) {
        result = xsx::network::acquire(cls, iid, output);
        std::printf("[XNETWORKING] loopback provider hr=0x%08lx\n", static_cast<unsigned long>(result));
    }
    return result;
}
template<class T> inline FARPROC procedure(T value) {
    FARPROC result;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
inline FARPROC WINAPI trace_resolve(HMODULE module, LPCSTR name) {
    FARPROC result = GetProcAddress(module, name);
    if (!result || reinterpret_cast<uintptr_t>(name) <= 0xffff) return result;
    if (std::strcmp(name, "InitializeApiImplEx2") == 0) {
        std::memcpy(&initialize, &result, sizeof(result));
        return procedure(&trace_initialize);
    }
    if (std::strcmp(name, "QueryApiImpl") == 0) {
        std::memcpy(&query, &result, sizeof(result));
        return procedure(&trace_query);
    }
    return result;
}

class Imports {
    ULONG_PTR* slot_ = nullptr;
    ULONG_PTR original_ = 0;
public:
    explicit Imports(HMODULE module) {
        auto* base = reinterpret_cast<uint8_t*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) throw std::runtime_error("XCurl DOS header");
        auto* pe = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE) throw std::runtime_error("XCurl PE header");
        auto directory = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; imports->Name; ++imports) {
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->OriginalFirstThunk);
            auto* functions = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++functions) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                auto* item = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(item->Name, "GetProcAddress") != 0) continue;
                slot_ = &functions->u1.Function;
                original_ = *slot_;
                DWORD protection;
                if (!VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection))
                    throw std::runtime_error("XCurl IAT protection");
                FARPROC replacement = procedure(&trace_resolve);
                std::memcpy(slot_, &replacement, sizeof(replacement));
                DWORD ignored; VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
                return;
            }
        }
        throw std::runtime_error("XCurl GetProcAddress import");
    }
    ~Imports() {
        if (!slot_) return;
        DWORD protection, ignored;
        if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection)) {
            *slot_ = original_;
            VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
        }
    }
};
} // namespace xcurl_trace
