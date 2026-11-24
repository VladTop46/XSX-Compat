// Проверка Windows-10 placeholder API в Wine: VirtualAlloc2(RESERVE|PLACEHOLDER), VirtualFree(PRESERVE_PLACEHOLDER) split, MapViewOfFile3(REPLACE_PLACEHOLDER).
#include <windows.h>
#include <cstdio>
typedef PVOID (WINAPI* VA2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef PVOID (WINAPI* MVF3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
int main()
{
    HMODULE kb = LoadLibraryW(L"kernelbase.dll"); auto va2 = (VA2)GetProcAddress(kb, "VirtualAlloc2"); auto mvf3 = (MVF3)GetProcAddress(kb, "MapViewOfFile3");
    printf("VirtualAlloc2=%p MapViewOfFile3=%p\n", (void*)va2, (void*)mvf3); if (!va2) return 2;
    const SIZE_T M = 1 << 20;
    char* p = (char*)va2(nullptr, nullptr, 16 * M, MEM_RESERVE | 0x40000 /*MEM_RESERVE_PLACEHOLDER*/, PAGE_NOACCESS, nullptr, 0);
    printf("1) reserve placeholder 16 MiB -> %p err=%lu\n", p, p ? 0 : GetLastError()); if (!p) return 1;
    BOOL ok = VirtualFree(p, 4 * M, MEM_RELEASE | 0x2 /*MEM_PRESERVE_PLACEHOLDER*/); printf("2) split VirtualFree(p, 4MiB, RELEASE|PRESERVE_PLACEHOLDER) -> %d err=%lu\n", ok, ok ? 0 : GetLastError());
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)(4 * M), nullptr);
    if (mvf3) { void* v = mvf3(sec, nullptr, p, 0, 4 * M, 0x4000 /*MEM_REPLACE_PLACEHOLDER*/, PAGE_READWRITE, nullptr, 0);
        printf("3) MapViewOfFile3(REPLACE_PLACEHOLDER) -> %p err=%lu\n", v, v ? 0 : GetLastError());
        if (v) { *(int*)v = 0x1234; printf("   write/read ok: %#x\n", *(int*)v); } }
    // коммит внутри placeholder'а нельзя; проверим освобождение остатка
    ok = VirtualFree(p + 4 * M, 0, MEM_RELEASE); printf("4) release remaining -> %d err=%lu\n", ok, ok ? 0 : GetLastError());
    return 0;
}
