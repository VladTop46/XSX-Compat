#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "ffx_api.h"
#include "ffx_upscale.h"

int main()
{
    HMODULE mod = LoadLibraryW(
        L"amd_fidelityfx_loader_xbox.dll");

    if (!mod) {
        printf("[FAIL] LoadLibrary: %lu\n", GetLastError());
        return 1;
    }

    auto query = reinterpret_cast<PfnFfxQuery>(
        GetProcAddress(mod, "ffxQuery"));

    if (!query) {
        puts("[FAIL] Missing ffxQuery");
        FreeLibrary(mod);
        return 2;
    }

    ffxQueryDescGetVersions desc{};
    desc.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    desc.createDescType =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;

    uint64_t count = 0;
    desc.outputCount = &count;

    auto rc = query(nullptr, &desc.header);

    printf("[COUNT] result=%d versions=%llu\n",
           (int)rc, (unsigned long long)count);

    if (rc != 0 || count == 0 || count > 64) {
        FreeLibrary(mod);
        return 3;
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count, nullptr);

    desc.versionIds = ids.data();
    desc.versionNames = names.data();

    uint64_t capacity = count;
    desc.outputCount = &capacity;

    rc = query(nullptr, &desc.header);

    printf("[VERSIONS] result=%d returned=%llu\n",
           (int)rc, (unsigned long long)capacity);

    if (rc == 0 && capacity <= count) {
        for (uint64_t i = 0; i < capacity; ++i) {
            printf("  [%llu] ID=0x%016llX NAME=%s\n",
                   (unsigned long long)i,
                   (unsigned long long)ids[i],
                   names[i] ? names[i] : "(null)");
        }
    }

    FreeLibrary(mod);
    return rc == 0 && capacity <= count ? 0 : 4;
}
