// Small GPU helpers for tests: compute dispatch that writes out[i] = 0xC0DE0000|i at any GPU VA.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "gpu_unix.h"
#include "phys_unix.h"
static const uint8_t k_store[] = { 0x02, 0x86, 0x03, 0x8f, 0x03, 0x00, 0x02, 0x4a, 0xff, 0x02, 0x04, 0x38, 0x00, 0x00, 0xde, 0xc0, 0x82, 0x02, 0x06, 0x34, 0x00, 0x80, 0x70, 0xdc, 0x03, 0x02, 0x00, 0x00, 0x70, 0x3f, 0x8c, 0xbf, 0x00, 0x00, 0x81, 0xbf };
#define PKT3(op, n) ((3u << 30) | (((n) - 1u) << 16) | ((op) << 8))
static uint32_t* ib; static unsigned ibn;
static void emit(uint32_t v) { ib[ibn++] = v; }
static void sh(uint32_t reg, unsigned n, const uint32_t* v) { emit(PKT3(0x76, n + 1)); emit((reg - 0xB000u) >> 2); for (unsigned i = 0; i < n; i++) emit(v[i]); }
static void sh1(uint32_t reg, uint32_t v) { sh(reg, 1, &v); }
int gpu_fill(amdgpu_device_handle dev, uint64_t out_va, unsigned groups)
{
    const size_t S = 1 << 16;
    uint8_t* m = mmap(NULL, S, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return -1;
    amdgpu_bo_handle bo;
    if (amdgpu_create_bo_from_user_mem(dev, m, S, &bo)) return -2;
    if (amdgpu_bo_va_op_raw(dev, bo, 0, S, (uint64_t)(uintptr_t)m, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP)) return -3;
    uint8_t* code = m + 0x8000; memcpy(code, k_store, sizeof k_store); memset(code + sizeof k_store, 0, 256);
    ib = (uint32_t*)m; ibn = 0;
    const uint64_t pc = (uint64_t)(uintptr_t)code;
    uint32_t pgm[2] = { (uint32_t)(pc >> 8), (uint32_t)(pc >> 40) }; sh(0xB830, 2, pgm);
    sh1(0xB848, 1u << 25); sh1(0xB84C, (2u << 1) | (1u << 7)); sh1(0xB8A0, 0); sh1(0xB860, 0); sh1(0xB854, 0);
    uint32_t mask[2] = { ~0u, ~0u }; sh(0xB858, 2, mask); sh(0xB864, 2, mask);
    uint32_t st[3] = { 0, 0, 0 }; sh(0xB810, 3, st); uint32_t nt[3] = { 64, 1, 1 }; sh(0xB81C, 3, nt);
    uint32_t ud[2] = { (uint32_t)out_va, (uint32_t)(out_va >> 32) }; sh(0xB900, 2, ud);
    emit(PKT3(0x15, 4)); emit(groups); emit(1); emit(1); emit(1u | (1u << 2) | (1u << 3) | (1u << 15));
    while (ibn % 8) emit(0xffff1000u);
    amdgpu_context_handle ctx; amdgpu_bo_list_handle list;
    if (amdgpu_cs_ctx_create(dev, &ctx)) return -4;
    static amdgpu_bo_handle all[1025]; all[0] = bo; unsigned n = 1 + phys_bo_list(all + 1, 1024);
    if (amdgpu_bo_list_create(dev, n, all, NULL, &list)) return -5;
    struct amdgpu_cs_ib_info ibi = { 0 }; ibi.ib_mc_address = (uint64_t)(uintptr_t)m; ibi.size = ibn;
    struct amdgpu_cs_request req = { 0 }; req.ip_type = AMDGPU_HW_IP_COMPUTE; req.resources = list; req.number_of_ibs = 1; req.ibs = &ibi;
    int r = amdgpu_cs_submit(ctx, 0, &req, 1); if (r) return -6;
    struct amdgpu_cs_fence f = { ctx, AMDGPU_HW_IP_COMPUTE, 0, 0, req.seq_no }; uint32_t exp = 0;
    r = amdgpu_cs_query_fence_status(&f, 2000000000ull, 0, &exp);
    amdgpu_bo_list_destroy(list); amdgpu_cs_ctx_free(ctx);
    amdgpu_bo_va_op_raw(dev, bo, 0, S, (uint64_t)(uintptr_t)m, 0, AMDGPU_VA_OP_UNMAP); amdgpu_bo_free(bo); munmap(m, S);
    return r || !exp ? -7 : 0;
}
