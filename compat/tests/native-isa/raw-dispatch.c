// Experiment 1: execute raw RDNA2 ISA on the host GPU through amdgpu PM4,
// with process memory mapped into the GPU VM at VA == CPU address
// ("identity mapping"), so pointers embedded by the shader/descriptors stay valid.
// Native Linux only; no Wine, no game code.
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

#define PKT3(op, n) ((3u << 30) | (((n) - 1u) << 16) | ((op) << 8))
#define PKT3_SET_SH_REG 0x76
#define PKT3_DISPATCH_DIRECT 0x15
#define SH_BASE 0xB000u
// GFX10 compute registers (Mesa sid.h)
#define COMPUTE_DISPATCH_INITIATOR 0xB800u
#define COMPUTE_START_X 0xB810u
#define COMPUTE_NUM_THREAD_X 0xB81Cu
#define COMPUTE_PGM_LO 0xB830u
#define COMPUTE_PGM_RSRC1 0xB848u
#define COMPUTE_PGM_RSRC2 0xB84Cu
#define COMPUTE_RESOURCE_LIMITS 0xB854u
#define COMPUTE_STATIC_THREAD_MGMT_SE0 0xB858u
#define COMPUTE_TMPRING_SIZE 0xB860u
#define COMPUTE_STATIC_THREAD_MGMT_SE2 0xB864u
#define COMPUTE_PGM_RSRC3 0xB8A0u
#define COMPUTE_USER_DATA_0 0xB900u

static uint32_t ib_dw[256]; static unsigned ib_n;
static void emit(uint32_t v) { ib_dw[ib_n++] = v; }
static void set_sh(uint32_t reg, unsigned n, const uint32_t* v)
{
    emit(PKT3(PKT3_SET_SH_REG, n + 1)); emit((reg - SH_BASE) >> 2);
    for (unsigned i = 0; i < n; i++) emit(v[i]);
}
static void set_sh1(uint32_t reg, uint32_t v) { set_sh(reg, 1, &v); }

static amdgpu_device_handle dev;
// Map [p, p+size) of this process at GPU VA == p.
static int map_identity(void* p, uint64_t size, amdgpu_bo_handle* bo)
{
    int r = amdgpu_create_bo_from_user_mem(dev, p, size, bo);
    if (r) { fprintf(stderr, "[FAIL] userptr bo %p size %#llx r=%d\n", p, (unsigned long long)size, r); return r; }
    r = amdgpu_bo_va_op_raw(dev, *bo, 0, size, (uint64_t)(uintptr_t)p,
                            AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
    if (r) fprintf(stderr, "[FAIL] identity VA map %p r=%d\n", p, r);
    return r;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s shader.bin\n", argv[0]); return 2; }
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("[FAIL] open renderD128"); return 1; }
    uint32_t maj, min;
    if (amdgpu_device_initialize(fd, &maj, &min, &dev)) { fprintf(stderr, "[FAIL] device init\n"); return 1; }
    struct amdgpu_gpu_info gi; amdgpu_query_gpu_info(dev, &gi);
    struct drm_amdgpu_info_device di; amdgpu_query_info(dev, AMDGPU_INFO_DEV_INFO, sizeof di, &di);
    printf("[INFO] family=%u chip_external_rev=%#x se=%u va=[%#llx,%#llx) high=[%#llx,%#llx)\n", gi.family_id, gi.chip_external_rev,
           gi.num_shader_engines, (unsigned long long)di.virtual_address_offset, (unsigned long long)di.virtual_address_max,
           (unsigned long long)di.high_va_offset, (unsigned long long)di.high_va_max);

    // Shader code + output buffer + IB: all ordinary process memory.
    const size_t page = 1 << 16;
    uint8_t* code = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t* out = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t* ibmem = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror("[FAIL] shader"); return 1; }
    size_t codeLen = fread(code, 1, page - 256, f); fclose(f);
    memset(code + codeLen, 0, 256); // s_code_end-style padding for the prefetcher
    for (unsigned i = 0; i < 4096; i++) out[i] = 0xdeadbeef;
    printf("[INFO] code=%p (%zu bytes) out=%p ib=%p\n", (void*)code, codeLen, (void*)out, (void*)ibmem);

    amdgpu_bo_handle bcode, bout, bib;
    if (map_identity(code, page, &bcode) || map_identity(out, page, &bout) || map_identity(ibmem, page, &bib)) return 1;

    const unsigned groups = 16, threads = 64, total = groups * threads;
    const uint64_t pc = (uint64_t)(uintptr_t)code, ob = (uint64_t)(uintptr_t)out;
    uint32_t pgm[2] = { (uint32_t)(pc >> 8), (uint32_t)(pc >> 40) };
    set_sh(COMPUTE_PGM_LO, 2, pgm);
    // RSRC1: VGPRS granule (wave32: 8 regs/granule) -> 4 VGPRs => 0; MEM_ORDERED(bit25)
    set_sh1(COMPUTE_PGM_RSRC1, (1u << 25));
    // RSRC2: USER_SGPR=2 (bits1..5), TGID_X_EN (bit7), TIDIG_COMP_CNT=0
    set_sh1(COMPUTE_PGM_RSRC2, (2u << 1) | (1u << 7));
    set_sh1(COMPUTE_PGM_RSRC3, 0);
    set_sh1(COMPUTE_TMPRING_SIZE, 0);
    set_sh1(COMPUTE_RESOURCE_LIMITS, 0);
    uint32_t mask2[2] = { 0xffffffff, 0xffffffff };
    set_sh(COMPUTE_STATIC_THREAD_MGMT_SE0, 2, mask2);
    set_sh(COMPUTE_STATIC_THREAD_MGMT_SE2, 2, mask2);
    uint32_t start[3] = { 0, 0, 0 }; set_sh(COMPUTE_START_X, 3, start);
    uint32_t nt[3] = { threads, 1, 1 }; set_sh(COMPUTE_NUM_THREAD_X, 3, nt);
    uint32_t ud[2] = { (uint32_t)ob, (uint32_t)(ob >> 32) }; set_sh(COMPUTE_USER_DATA_0, 2, ud);
    // DISPATCH_DIRECT: COMPUTE_SHADER_EN | FORCE_START_AT_000 | ORDER_MODE(bit3) | CS_W32_EN(bit15)
    emit(PKT3(PKT3_DISPATCH_DIRECT, 4)); emit(groups); emit(1); emit(1);
    emit(1u | (1u << 2) | (1u << 3) | (1u << 15));
    while (ib_n % 8) emit(0xffff1000u); // type-2 NOP padding
    memcpy(ibmem, ib_dw, ib_n * 4);

    amdgpu_context_handle ctx;
    if (amdgpu_cs_ctx_create(dev, &ctx)) { fprintf(stderr, "[FAIL] ctx\n"); return 1; }
    amdgpu_bo_handle all[3] = { bcode, bout, bib };
    amdgpu_bo_list_handle list;
    if (amdgpu_bo_list_create(dev, 3, all, NULL, &list)) { fprintf(stderr, "[FAIL] bo list\n"); return 1; }
    struct amdgpu_cs_ib_info ib = { 0 }; ib.ib_mc_address = (uint64_t)(uintptr_t)ibmem; ib.size = ib_n;
    struct amdgpu_cs_request req = { 0 };
    req.ip_type = AMDGPU_HW_IP_COMPUTE; req.resources = list; req.number_of_ibs = 1; req.ibs = &ib;
    int r = amdgpu_cs_submit(ctx, 0, &req, 1);
    if (r) { fprintf(stderr, "[FAIL] submit r=%d\n", r); return 1; }
    struct amdgpu_cs_fence fence = { ctx, AMDGPU_HW_IP_COMPUTE, 0, 0, req.seq_no };
    uint32_t expired = 0;
    r = amdgpu_cs_query_fence_status(&fence, 2000000000ull, 0, &expired);
    printf("[INFO] fence r=%d expired=%u ib_dwords=%u\n", r, expired, ib_n);
    if (r || !expired) { fprintf(stderr, "[FAIL] GPU did not complete\n"); return 1; }

    unsigned ok = 0, bad = 0;
    for (unsigned i = 0; i < total; i++) { if (out[i] == (0xC0DE0000u | i)) ok++; else if (bad++ < 4) printf("[MISMATCH] out[%u]=%#x\n", i, out[i]); }
    unsigned guard = 0; for (unsigned i = total; i < 4096; i++) guard += out[i] != 0xdeadbeef;
    printf("[%s] raw RDNA2 ISA dispatch: ok=%u/%u bad=%u guard_errors=%u (identity VA mapping of process memory)\n",
           ok == total && !guard ? "PASS" : "FAIL", ok, total, bad, guard);
    amdgpu_bo_list_destroy(list); amdgpu_cs_ctx_free(ctx);
    return ok == total && !guard ? 0 : 1;
}
