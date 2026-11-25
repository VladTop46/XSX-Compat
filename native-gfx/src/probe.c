#include <windows.h>
#include <stdio.h>
int main(void)
{
    HMODULE m = LoadLibraryA(__argc > 1 ? __argv[1] : "xsxgpu.dll");
    if (!m) { printf("[FAIL] LoadLibrary xsxgpu err=%lu\n", GetLastError()); return 1; }
    int (WINAPI *open_)(unsigned*, unsigned*, unsigned long long*) = (void*)GetProcAddress(m, "xsxgpu_open");
    unsigned fam = 0, dev = 0; unsigned long long va = 0;
    int r = open_ ? open_(&fam, &dev, &va) : -200;
    printf("[%s] xsxgpu_open from Wine process: r=%d family=%u device=%#x va_max=%#llx\n", r ? "FAIL" : "PASS", r, fam, dev, va);
    if (r) return 1;
    // Arena from the Wine process' own allocator; PS descriptor tables must be < 4 GiB.
    void* arena = VirtualAlloc((void*)0x10000000, 4u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!arena) arena = VirtualAlloc(NULL, 4u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    printf("[INFO] arena=%p (VirtualAlloc in Wine)\n", arena); fflush(stdout);
    int (WINAPI *exp2)(void*, unsigned long long) = (void*)GetProcAddress(m, "xsxgpu_exp2");
    int e = exp2 ? exp2(arena, 4u << 20) : -200;
    printf("[%s] exp2 (game VS_Blit+PS_BlitColour) inside Wine process r=%d\n", e ? "FAIL" : "PASS", e);
    return e ? 1 : 0;
}
