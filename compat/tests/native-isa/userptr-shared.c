// De-risk: can amdgpu userptr map SHARED (section-like) memory, incl. two aliased views
// of the same pages and a regular-file backing (Wine may back sections with temp files)?
#define _GNU_SOURCE
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static amdgpu_device_handle dev;
static int try_map(const char* what, void* p, size_t size)
{
    amdgpu_bo_handle bo; int r = amdgpu_create_bo_from_user_mem(dev, p, size, &bo);
    int r2 = r ? -1 : amdgpu_bo_va_op_raw(dev, bo, 0, size, (uint64_t)(uintptr_t)p, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE, AMDGPU_VA_OP_MAP);
    printf("[%s] %s userptr=%d vamap=%d at %p\n", !r && !r2 ? "OK" : "FAIL", what, r, r2, p);
    return !r && !r2;
}
int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR); uint32_t a, b;
    if (amdgpu_device_initialize(fd, &a, &b, &dev)) return 1;
    const size_t S = 1 << 20;
    int mf = memfd_create("xmem-test", 0); ftruncate(mf, S);
    void* v1 = mmap((void*)0x20000000, S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, mf, 0);
    void* v2 = mmap((void*)0x30000000, S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, mf, 0);
    int ok = try_map("memfd view1", v1, S) & try_map("memfd view2 (alias)", v2, S);
    char path[] = "/tmp/claude-1000/-home-vladtop46-xsx-research/fdf9c174-7279-442b-a3ae-08aae81a2f34/scratchpad/secXXXXXX";
    int tf = mkstemp(path); unlink(path); ftruncate(tf, S);
    void* v3 = mmap((void*)0x40000000, S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, tf, 0);
    ok &= try_map("tmpfile shared", v3, S);
    void* v4 = mmap((void*)0x50000000, S, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    ok &= try_map("anon private", v4, S);
    return ok ? 0 : 1;
}
