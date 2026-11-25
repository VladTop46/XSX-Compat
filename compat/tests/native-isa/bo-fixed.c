// Inverse scheme: physical pages = amdgpu GTT BO; CPU maps it at FIXED guest addresses
// (two aliased views via the DRM fd mmap offset), GPU maps the same BO at the same VAs.
// Then: GPU writes through alias A, CPU reads through alias B.
#define _GNU_SOURCE
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>
int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR); uint32_t a, b; amdgpu_device_handle dev;
    if (amdgpu_device_initialize(fd, &a, &b, &dev)) return 1;
    const size_t S = 1 << 20;
    void* sh = mmap((void*)0x60000000, S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    amdgpu_bo_handle t; printf("[%s] anon shared userptr r=%d\n", amdgpu_create_bo_from_user_mem(dev, sh, S, &t) ? "FAIL" : "OK", 0);
    struct amdgpu_bo_alloc_request req = { .alloc_size = S, .phys_alignment = 1 << 16, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT, .flags = AMDGPU_GEM_CREATE_CPU_GTT_USWC * 0 };
    amdgpu_bo_handle bo; if (amdgpu_bo_alloc(dev, &req, &bo)) { puts("[FAIL] bo alloc"); return 1; }
    uint32_t kms; amdgpu_bo_export(bo, amdgpu_bo_handle_type_kms, &kms);
    union drm_amdgpu_gem_mmap mm = { .in.handle = kms };
    if (drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &mm)) { puts("[FAIL] gem mmap offset"); return 1; }
    void* A = mmap((void*)0x70000000, S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, mm.out.addr_ptr);
    void* B = mmap((void*)0x80000000, S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, mm.out.addr_ptr);
    printf("[%s] CPU fixed views A=%p B=%p\n", A == (void*)0x70000000 && B == (void*)0x80000000 ? "OK" : "FAIL", A, B);
    int r1 = amdgpu_bo_va_op_raw(dev, bo, 0, S, 0x70000000, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
    int r2 = amdgpu_bo_va_op_raw(dev, bo, 0, S, 0x80000000, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
    printf("[%s] GPU VA maps at same addresses r=%d,%d\n", !r1 && !r2 ? "OK" : "FAIL", r1, r2);
    ((volatile uint32_t*)A)[5] = 0x12345678;
    printf("[%s] CPU alias coherence A->B %#x\n", ((volatile uint32_t*)B)[5] == 0x12345678 ? "OK" : "FAIL", ((volatile uint32_t*)B)[5]);
    return 0;
}
