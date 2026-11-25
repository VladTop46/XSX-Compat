// M2 probe (Wine): XMem-like placeholder reservation, BO-backed physical pages mapped with an
// alias, CPU coherence across aliases, GPU compute write through alias B read via alias A,
// unmap -> access faults, remap of a different id.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef int (WINAPI *open_t)(unsigned*, unsigned*, unsigned long long*);
typedef int (WINAPI *map_t)(void*, unsigned long long, const ULONG_PTR*, unsigned long long);
typedef int (WINAPI *unmap_t)(void*, unsigned long long);
typedef int (WINAPI *fill_t)(void*, unsigned);
typedef void (WINAPI *stats_t)(unsigned long long*, unsigned long long*);
static unsigned checks, fails;
static void check(int ok, const char* what) { checks++; if (!ok) { fails++; printf("[FAIL] %s\n", what); } else printf("[ok] %s\n", what); }
static int probe_read(volatile uint32_t* p) { return !IsBadReadPtr((const void*)p, 4); }
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE m = LoadLibraryA("xsxgpu.dll"); if (!m) { printf("[FAIL] load\n"); return 1; }
    open_t op = (open_t)GetProcAddress(m, "xsxgpu_open"); map_t map = (map_t)GetProcAddress(m, "xsxgpu_phys_map");
    unmap_t unmap = (unmap_t)GetProcAddress(m, "xsxgpu_phys_unmap"); fill_t fill = (fill_t)GetProcAddress(m, "xsxgpu_gpu_fill");
    stats_t stats = (stats_t)GetProcAddress(m, "xsxgpu_phys_stats");
    unsigned f, d; unsigned long long va; check(op(&f, &d, &va) == 0, "open amdgpu");
    // Reserve like XMemVirtualAlloc(PHYSICAL): plain reservation, no commit.
    const SIZE_T P = 0x10000; uint8_t* region = VirtualAlloc(NULL, 64 * P, MEM_RESERVE, PAGE_READWRITE);
    check(region != NULL, "reserve 4 MiB region"); printf("[INFO] region=%p\n", region);
    ULONG_PTR ids[4] = { 0x100000, 0x100010, 0x100020, 0x100030 };
    uint8_t* A = region; uint8_t* B = region + 32 * P;
    { int r = map(A, 4, ids, 1); printf("[INFO] map A r=%d\n", r); check(r == 0, "map 4 pages at A"); }
    check(map(B, 4, ids, 1) == 0, "map the same 4 pages at B (alias)");
    ((volatile uint32_t*)A)[7] = 0xfeedf00d; check(((volatile uint32_t*)B)[7] == 0xfeedf00d, "CPU alias coherence A->B");
    check(fill(B + P, 4) == 0, "GPU compute writes 256 dwords via alias B (page 1)");
    int gpuOk = 1; for (int i = 0; i < 256; i++) gpuOk &= ((volatile uint32_t*)(A + P))[i] == (0xC0DE0000u | i);
    check(gpuOk, "CPU reads GPU result via alias A");
    MEMORY_BASIC_INFORMATION mbi; VirtualQuery(A, &mbi, sizeof mbi);
    printf("[INFO] Wine VirtualQuery(A): State=%#lx Protect=%#lx (Wine bookkeeping is unaware of the overlay)\n", mbi.State, mbi.Protect);
    { uint32_t v = 0; SIZE_T got = 0; BOOL ok = ReadProcessMemory(GetCurrentProcess(), A + 28, &v, 4, &got);
      check(ok && got == 4 && v == 0xfeedf00d, "ReadProcessMemory(self) on overlay page");
      check(!IsBadReadPtr(A, 4), "IsBadReadPtr(A) == FALSE on overlay page"); }
    check(unmap(B, 4) == 0, "unmap alias B");
    check(!probe_read((volatile uint32_t*)B), "access to unmapped B faults");
    check(((volatile uint32_t*)A)[7] == 0xfeedf00d, "A still intact after B unmap");
    ULONG_PTR other = 0x100000 + 16 * 600; // second chunk BO
    check(map(B, 1, &other, 1) == 0, "remap B to an id in another BO chunk");
    ((volatile uint32_t*)B)[0] = 0x1234; check(((volatile uint32_t*)A)[0] != 0x1234, "new page is distinct from A");
    unsigned long long mapped, bytes; stats(&mapped, &bytes); printf("[INFO] mapped_pages_total=%llu bo_bytes=%llu MiB\n", mapped, bytes >> 20);
    printf("[%s] M2 phys pages: checks=%u fails=%u\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
