// Experiment 3: textured draw with the game's own ISA: sga_dr_vs_pos_color_tex
// (world*viewproj from two V# constant buffers) + default_ps_tex (image_sample
// through a T# from the descriptor table, inline S# in the shader) on the host
// RDNA2 GPU through raw PM4 on the GFX ring. Xbox RECTLIST topology is used
// natively. RSRC1/RSRC2 and CB_COLOR_CONTROL values come from the captured PSO
// state block (pso1_q1.bin). All memory is ordinary process memory mapped into
// the GPU VM at VA == CPU address, below 1 TiB (VS takes a 40-bit table pointer,
// PS a 32-bit one). Native Linux; no Wine, no game process.
#define _GNU_SOURCE
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PKT3(op, n) ((3u << 30) | (((n) - 1u) << 16) | ((op) << 8))
enum { OP_CONTEXT_CONTROL = 0x28, OP_CLEAR_STATE = 0x12, OP_SET_CONTEXT_REG = 0x69, OP_SET_SH_REG = 0x76,
       OP_SET_UCONFIG_REG = 0x79, OP_SET_UCONFIG_REG_INDEX = 0x7A, OP_NUM_INSTANCES = 0x2F, OP_DRAW_INDEX_AUTO = 0x2D };
// gc_10_3_0_offset.h: context (BASE_IDX 1) offsets are packet offsets directly;
// SH (BASE_IDX 0) packet offset = off - 0x19A0; uconfig = off - 0x2000.
#define SH(off) ((off) - 0x19A0u)
#define UC(off) ((off) - 0x2000u)
enum {
    PA_CL_VTE_CNTL = 0x206, PA_CL_VPORT_XSCALE = 0x10f, PA_SC_VPORT_ZMIN_0 = 0x0b4, PA_CL_CLIP_CNTL = 0x204,
    PA_SU_SC_MODE_CNTL = 0x205, PA_SU_VTX_CNTL = 0x2f9, PA_CL_GB_VERT_CLIP_ADJ = 0x2fa, PA_SC_SCREEN_SCISSOR_TL = 0x00c,
    PA_SC_WINDOW_SCISSOR_TL = 0x081, PA_SC_GENERIC_SCISSOR_TL = 0x090, PA_SC_VPORT_SCISSOR_0_TL = 0x094,
    PA_SC_CLIPRECT_RULE = 0x083, PA_SC_AA_MASK_X0Y0_X1Y0 = 0x30e, PA_SC_BINNER_CNTL_0 = 0x311,
    DB_RENDER_CONTROL = 0x000, DB_Z_INFO = 0x010, DB_STENCIL_INFO = 0x011, DB_DEPTH_CONTROL = 0x200, DB_SHADER_CONTROL = 0x203,
    CB_COLOR0_BASE = 0x318, CB_COLOR0_VIEW = 0x31b, CB_COLOR0_INFO = 0x31c, CB_COLOR0_ATTRIB = 0x31d,
    CB_COLOR0_ATTRIB2 = 0x3b0, CB_COLOR0_ATTRIB3 = 0x3b8, CB_COLOR0_BASE_EXT = 0x390,
    CB_TARGET_MASK = 0x08e, CB_SHADER_MASK = 0x08f, CB_COLOR_CONTROL = 0x202, CB_BLEND0_CONTROL = 0x1e0,
    SPI_SHADER_COL_FORMAT = 0x1c5, SPI_SHADER_Z_FORMAT = 0x1c4, SPI_PS_INPUT_ENA = 0x1b3, SPI_PS_INPUT_ADDR = 0x1b4,
    SPI_PS_IN_CONTROL = 0x1b6, SPI_PS_INPUT_CNTL_0 = 0x191, SPI_VS_OUT_CONFIG = 0x1b1, SPI_SHADER_POS_FORMAT = 0x1c3,
    PA_CL_VS_OUT_CNTL = 0x207, VGT_SHADER_STAGES_EN = 0x2d5, VGT_GS_MODE = 0x290, SX_PS_DOWNCONVERT = 0x1d5,
    GE_CNTL = 0x225b, VGT_PRIMITIVE_TYPE = 0x2242, GE_PC_ALLOC = 0x2260,
    SPI_SHADER_PGM_LO_VS = 0x19e8, SPI_SHADER_PGM_RSRC1_VS = 0x19ea, SPI_SHADER_PGM_RSRC3_VS = 0x19e6,
    SPI_SHADER_PGM_RSRC4_VS = 0x19e1, SPI_SHADER_USER_DATA_VS_0 = 0x19ec, SPI_SHADER_LATE_ALLOC_VS = 0x19e7,
    SPI_SHADER_PGM_LO_PS = 0x19a8, SPI_SHADER_PGM_RSRC1_PS = 0x19aa, SPI_SHADER_PGM_RSRC3_PS = 0x19a7,
    SPI_SHADER_PGM_RSRC4_PS = 0x19a1, SPI_SHADER_USER_DATA_PS_0 = 0x19ac,
};

static uint32_t* ib; static unsigned ibn;
static void emit(uint32_t v) { ib[ibn++] = v; }
static void setreg(uint32_t op, uint32_t off, unsigned n, const uint32_t* v)
{ emit(PKT3(op, n + 1)); emit(off); for (unsigned i = 0; i < n; i++) emit(v[i]); }
static void ctx(uint32_t off, uint32_t v) { setreg(OP_SET_CONTEXT_REG, off, 1, &v); }
static void ctxn(uint32_t off, unsigned n, const uint32_t* v) { setreg(OP_SET_CONTEXT_REG, off, n, v); }
static void sh(uint32_t off, uint32_t v) { setreg(OP_SET_SH_REG, SH(off), 1, &v); }
static void shn(uint32_t off, unsigned n, const uint32_t* v) { setreg(OP_SET_SH_REG, SH(off), n, v); }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static size_t load(const char* path, void* dst, size_t max)
{
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    size_t n = fread(dst, 1, max, f); fclose(f); return n;
}
static uint32_t pso_word(const uint8_t* blob, unsigned off) { uint32_t v; memcpy(&v, blob + off, 4); return v; }
// RNE float -> fp16 -> float (RTZ for pkrtz); compute oracle for one channel
static float half_rtz(float x)
{
    if (x == 0) return 0; int e; float m = frexpf(x, &e); // x = m * 2^e, m in [0.5,1)
    float scaled = ldexpf(m, 11); scaled = truncf(scaled); return ldexpf(scaled, e - 11);
}

int main(int argc, char** argv)
{
    int nopPrefix = 0;
    const int negative = argc > 1 && !strcmp(argv[1], "--negative"); // shifts geometry by 8 px: oracle MUST fail
    const char* dir = "isa";
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    amdgpu_device_handle dev; uint32_t maj, min;
    if (fd < 0 || amdgpu_device_initialize(fd, &maj, &min, &dev)) { fprintf(stderr, "[FAIL] device\n"); return 1; }

    // One 4 MiB arena at a fixed low address (< 4 GiB, so both 32-bit and 40-bit pointers work).
    const uint64_t BASE = 0x10000000ull, SIZE = 4u << 20;
    uint8_t* m = mmap((void*)BASE, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (uint8_t*)BASE) { perror("[FAIL] mmap fixed"); return 1; }
    memset(m, 0, SIZE);
    amdgpu_bo_handle bo;
    if (amdgpu_create_bo_from_user_mem(dev, m, SIZE, &bo) ||
        amdgpu_bo_va_op_raw(dev, bo, 0, SIZE, BASE, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP)) {
        fprintf(stderr, "[FAIL] identity map\n"); return 1;
    }
    ib = (uint32_t*)(m + 0x0);
    uint8_t* vs = m + 0x10000; uint8_t* fetch = m + 0x11000; uint8_t* ps = m + 0x12000;
    uint32_t* vsTable = (uint32_t*)(m + 0x20000); uint32_t* psTable = (uint32_t*)(m + 0x20100);
    float* vb = (float*)(m + 0x30000); float* cb = (float*)(m + 0x31000);
    uint32_t* rt = (uint32_t*)(m + 0x100000);
    const unsigned W = 64, H = 64, RT_WORDS = W * H, GUARD = 1024;

    char p[256]; uint8_t state[388];
    snprintf(p, sizeof p, "%s/04cf10b425938ecca5b19f2ae14592ab.isa", dir); size_t vsLen = load(p, vs, 0x1000);
    snprintf(p, sizeof p, "%s/b2e813c0805d55cf8c1656ea3e1a1f52.isa", dir); size_t psLen = load(p, ps, 0x1000);
    load("fetch.bin", fetch, 0x1000);
    if (load("../../../local-agent-work/data/pso1_q1.bin", state, sizeof state) != sizeof state) { fprintf(stderr, "[FAIL] state\n"); return 1; }
    if (nopPrefix) for (int i = 0; i < 24; i += 4) { const uint32_t snop = 0xbf800000u; memcpy(vs + i, &snop, 4); memcpy(ps + i, &snop, 4); }
    printf("[INFO] vs=%zu ps=%zu bytes negative_control=%d\n", vsLen, psLen, negative);

    // Values from the captured PSO state block (offsets inferred by field fit, see README).
    const uint32_t rsrc1Vs = pso_word(state, 0x38), rsrc2Vs = pso_word(state, 0x3c);
    const uint32_t rsrc1Ps = pso_word(state, 0x4c), rsrc2Ps = pso_word(state, 0x50);
    const uint32_t cbColorControl = pso_word(state, 0x7c);
    printf("[INFO] state: RSRC1_VS=%08x RSRC2_VS=%08x RSRC1_PS=%08x RSRC2_PS=%08x CB_COLOR_CONTROL=%08x\n", rsrc1Vs, rsrc2Vs, rsrc1Ps, rsrc2Ps, cbColorControl);

    // Game draw B geometry (live log): RECTLIST in pixel space, white, uv 0..1, stride 24.
    const float verts[3][6] = { { 0, 0, 0, 0, 0, 0 }, { 0, (float)H, 0, 0, 0, 1 }, { (float)W, (float)H, 0, 0, 1, 1 } };
    for (int i = 0; i < 3; i++) { memcpy(vb + 6 * i, verts[i], 24); ((uint32_t*)vb)[6 * i + 3] = 0xffffffffu; }
    // Buf0 world (rows at 0x00,0x10,0x20, translation 0x30) = identity; Buf1 viewproj at 0x160
    // (pixel -> NDC, the game's 3840x2160 values scaled to WxH).
    float* world = cb; float* vpb = cb + 256; // Buf1 at cb+1KiB
    world[0] = 1; world[5] = 1; world[10] = 1; world[12] = negative ? 8.f : 0.f;
    float* vpm = vpb + 0x160 / 4; vpm[0] = 2.f / W; vpm[5] = -2.f / H; vpm[10] = 1; vpm[12] = -1; vpm[13] = 1; vpm[15] = 1;
    // Texture: 64x64 linear RGBA8, distinct texels.
    uint32_t* tex = (uint32_t*)(m + 0x200000);
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++)
        tex[y * W + x] = (x * 4) | ((y * 4) << 8) | ((((x ^ y) * 4) & 255) << 16) | (255u << 24);
    const uint64_t vbVa = (uint64_t)(uintptr_t)vb, cbVa = (uint64_t)(uintptr_t)cb, vpVa = (uint64_t)(uintptr_t)vpb, texVa = (uint64_t)(uintptr_t)tex;
    const uint32_t rawW3 = 0xfac | (22u << 12) | (1u << 24) | (3u << 28);
    vsTable[0] = (uint32_t)vbVa; vsTable[1] = (uint32_t)(vbVa >> 32) | (24u << 16); vsTable[2] = 3; vsTable[3] = 0xfac | (22u << 12) | (1u << 24);
    // Resource table (shared by VS s8 and PS s8): +0x00 V# Buf0, +0x20 T#, +0x40 V# Buf1.
    psTable[0] = (uint32_t)cbVa; psTable[1] = (uint32_t)(cbVa >> 32); psTable[2] = 64; psTable[3] = rawW3;
    psTable[16] = (uint32_t)vpVa; psTable[17] = (uint32_t)(vpVa >> 32); psTable[18] = 0x1a0; psTable[19] = rawW3;
    // GFX10 T#: FORMAT 8_8_8_8_UNORM(56), WIDTH/HEIGHT, DST_SEL xyzw, SW_MODE 0 (linear), TYPE 2D(9), RESOURCE_LEVEL.
    uint32_t* T = psTable + 8;
    T[0] = (uint32_t)(texVa >> 8); T[1] = (uint32_t)(texVa >> 40) | (56u << 20) | (((W - 1) & 3) << 30);
    T[2] = ((W - 1) >> 2) | ((H - 1) << 14) | (1u << 31);
    T[3] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (0u << 20) | (9u << 28);
    T[4] = 0; T[5] = 0; T[6] = 0; T[7] = 0;
    for (unsigned i = 0; i < RT_WORDS + GUARD; i++) rt[i] = 0xcdcdcdcdu;

    emit(PKT3(OP_CONTEXT_CONTROL, 2)); emit(0x80000000u); emit(0x80000000u);
    emit(PKT3(OP_CLEAR_STATE, 1)); emit(0);
    // Shaders
    const uint64_t vsVa = (uint64_t)(uintptr_t)vs, psVa = (uint64_t)(uintptr_t)ps, fetchVa = (uint64_t)(uintptr_t)fetch;
    uint32_t vsPgm[4] = { (uint32_t)(vsVa >> 8), (uint32_t)(vsVa >> 40), rsrc1Vs, rsrc2Vs }; shn(SPI_SHADER_PGM_LO_VS, 4, vsPgm);
    uint32_t psPgm[4] = { (uint32_t)(psVa >> 8), (uint32_t)(psVa >> 40), rsrc1Ps, rsrc2Ps }; shn(SPI_SHADER_PGM_LO_PS, 4, psPgm);
    sh(SPI_SHADER_PGM_RSRC3_VS, 0xffffu | (0x3fu << 16)); sh(SPI_SHADER_PGM_RSRC4_VS, 0xffffu);
    sh(SPI_SHADER_PGM_RSRC3_PS, 0xffffu | (0x3fu << 16)); sh(SPI_SHADER_PGM_RSRC4_PS, 0xffffu);
    sh(SPI_SHADER_LATE_ALLOC_VS, 0);
    const uint64_t vtab = (uint64_t)(uintptr_t)vsTable;
    uint32_t vsUser[9] = { (uint32_t)vtab, (uint32_t)(vtab >> 32), 0 /*base vertex*/, 0, (uint32_t)fetchVa, (uint32_t)(fetchVa >> 32), 0, 0, (uint32_t)(uintptr_t)psTable };
    shn(SPI_SHADER_USER_DATA_VS_0, 9, vsUser);
    uint32_t psUser[9] = { 0, 0, 0, 0, 0, 0, 0, 0, (uint32_t)(uintptr_t)psTable };
    shn(SPI_SHADER_USER_DATA_PS_0, 9, psUser);
    // Pipeline (legacy VS, wave64)
    ctx(VGT_SHADER_STAGES_EN, 2u << 15); ctx(VGT_GS_MODE, 0);
    ctx(SPI_VS_OUT_CONFIG, 1u << 1); ctx(SPI_SHADER_POS_FORMAT, 4); ctx(PA_CL_VS_OUT_CNTL, 0);
    ctx(SPI_PS_INPUT_ENA, pso_word(state, 0x58)); ctx(SPI_PS_INPUT_ADDR, pso_word(state, 0x58)); ctx(SPI_PS_IN_CONTROL, pso_word(state, 0x5c)); ctx(SPI_PS_INPUT_CNTL_0, 0); ctx(SPI_PS_INPUT_CNTL_0 + 1, 1);
    ctx(SPI_SHADER_Z_FORMAT, 0); ctx(SPI_SHADER_COL_FORMAT, 4 /*FP16_ABGR*/);
    ctx(DB_SHADER_CONTROL, 0); ctx(DB_RENDER_CONTROL, 0); ctx(DB_Z_INFO, 0); ctx(DB_STENCIL_INFO, 0); ctx(DB_DEPTH_CONTROL, 0);
    // Raster
    ctx(PA_CL_CLIP_CNTL, (1u << 16) | (1u << 19)); ctx(PA_SU_SC_MODE_CNTL, 0);
    ctx(PA_SU_VTX_CNTL, 1u | (2u << 1) | (5u << 3));
    uint32_t gb[4] = { fbits(1), fbits(1), fbits(1), fbits(1) }; ctxn(PA_CL_GB_VERT_CLIP_ADJ, 4, gb);
    ctx(PA_CL_VTE_CNTL, 0x43f);
    uint32_t vp[6] = { fbits(W / 2.f), fbits(W / 2.f), fbits(-(H / 2.f)), fbits(H / 2.f), fbits(1), fbits(0) }; ctxn(PA_CL_VPORT_XSCALE, 6, vp);
    uint32_t z[2] = { fbits(0), fbits(1) }; ctxn(PA_SC_VPORT_ZMIN_0, 2, z);
    const uint32_t br = W | (H << 16);
    uint32_t sc[2] = { 0, br }; ctxn(PA_SC_SCREEN_SCISSOR_TL, 2, sc);
    uint32_t wsc[2] = { 0x80000000u, br }; ctxn(PA_SC_WINDOW_SCISSOR_TL, 2, wsc);
    ctx(PA_SC_CLIPRECT_RULE, 0xffff);
    ctxn(PA_SC_GENERIC_SCISSOR_TL, 2, wsc); ctxn(PA_SC_VPORT_SCISSOR_0_TL, 2, wsc);
    uint32_t aa[2] = { 0xffffffffu, 0xffffffffu }; ctxn(PA_SC_AA_MASK_X0Y0_X1Y0, 2, aa);
    ctx(PA_SC_BINNER_CNTL_0, 3u /*DISABLE_BINNING_USE_LEGACY_SC*/ | (1u << 18));
    // Color target: linear RGBA8 64x64
    const uint64_t rtVa = (uint64_t)(uintptr_t)rt;
    ctx(CB_COLOR0_BASE, (uint32_t)(rtVa >> 8)); ctx(CB_COLOR0_BASE_EXT, (uint32_t)(rtVa >> 40));
    ctx(CB_COLOR0_VIEW, 0); ctx(CB_COLOR0_INFO, 0xAu << 2 /*COLOR_8_8_8_8, UNORM, SWAP_STD*/);
    ctx(CB_COLOR0_ATTRIB, 0);
    ctx(CB_COLOR0_ATTRIB2, (H - 1) | ((W - 1) << 14));
    ctx(CB_COLOR0_ATTRIB3, (1u << 24) /*2D*/ | (1u << 27) /*RESOURCE_LEVEL*/);
    ctx(CB_TARGET_MASK, 0xf); ctx(CB_SHADER_MASK, 0xf); ctx(CB_COLOR_CONTROL, cbColorControl);
    ctx(CB_BLEND0_CONTROL, 0); ctx(SX_PS_DOWNCONVERT, 0);
    // Uconfig
    setreg(OP_SET_UCONFIG_REG, UC(GE_CNTL), 1, (uint32_t[]){ 128u | (256u << 9) });
    setreg(OP_SET_UCONFIG_REG, UC(GE_PC_ALLOC), 1, (uint32_t[]){ 1u | (255u << 1) });
    setreg(OP_SET_UCONFIG_REG_INDEX, UC(VGT_PRIMITIVE_TYPE) | (1u << 28), 1, (uint32_t[]){ 0x11 /*RECTLIST*/ });
    emit(PKT3(OP_NUM_INSTANCES, 1)); emit(1);
    emit(PKT3(OP_DRAW_INDEX_AUTO, 2)); emit(3); emit(2 /*SOURCE_SELECT=AUTO_INDEX*/);
    while (ibn % 8) emit(0xffff1000u);

    amdgpu_context_handle cx; amdgpu_bo_list_handle list;
    if (amdgpu_cs_ctx_create(dev, &cx) || amdgpu_bo_list_create(dev, 1, &bo, NULL, &list)) { fprintf(stderr, "[FAIL] ctx/list\n"); return 1; }
    struct amdgpu_cs_ib_info ibi = { 0 }; ibi.ib_mc_address = BASE; ibi.size = ibn;
    struct amdgpu_cs_request req = { 0 }; req.ip_type = AMDGPU_HW_IP_GFX; req.resources = list; req.number_of_ibs = 1; req.ibs = &ibi;
    int r = amdgpu_cs_submit(cx, 0, &req, 1);
    if (r) { fprintf(stderr, "[FAIL] submit r=%d\n", r); return 1; }
    struct amdgpu_cs_fence fence = { cx, AMDGPU_HW_IP_GFX, 0, 0, req.seq_no }; uint32_t expired = 0;
    r = amdgpu_cs_query_fence_status(&fence, 3000000000ull, 0, &expired);
    uint32_t hang = 0; amdgpu_cs_query_reset_state(cx, &hang, &(uint32_t){0});
    printf("[INFO] ib_dwords=%u fence r=%d expired=%u reset_state=%u\n", ibn, r, expired, hang);
    if (r || !expired) { fprintf(stderr, "[FAIL] GPU did not complete\n"); return 1; }

    // Oracle: texel(x,y) * white, pkrtz fp16, UNORM8. Pixel centers hit texel centers exactly.
    unsigned ok = 0, untouched = 0, maxErr = 0, guard = 0;
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        const uint8_t* t = (const uint8_t*)&tex[y * W + x]; const uint8_t* px = (const uint8_t*)&rt[y * W + x]; unsigned e = 0;
        for (int c = 0; c < 4; c++) { const uint8_t w = (uint8_t)lrintf(half_rtz(t[c] / 255.f) * 255.f);
            unsigned d = (unsigned)abs((int)px[c] - (int)w); if (d > e) e = d; }
        if (rt[y * W + x] == 0xcdcdcdcdu) untouched++;
        if (e <= 1) ok++; if (e > maxErr) maxErr = e;
    }
    for (unsigned i = RT_WORDS; i < RT_WORDS + GUARD; i++) guard += rt[i] != 0xcdcdcdcdu;
    const unsigned probe[3][2] = { { 0, 0 }, { 37, 21 }, { W - 1, H - 1 } };
    for (int k = 0; k < 3; k++) { const unsigned x = probe[k][0], y = probe[k][1]; const uint8_t* px = (const uint8_t*)&rt[y * W + x]; const uint8_t* t = (const uint8_t*)&tex[y * W + x];
        printf("[INFO] (%u,%u) tex=%u,%u,%u,%u out=%u,%u,%u,%u\n", x, y, t[0], t[1], t[2], t[3], px[0], px[1], px[2], px[3]); }
    const int pass = ok == RT_WORDS && !guard;
    printf("[%s] native game ISA textured draw (sga_dr_vs_pos_color_tex+default_ps_tex, T#, RECTLIST)%s: ok=%u/%u untouched=%u max_error=%u guard_errors=%u\n",
           pass ? "PASS" : "FAIL", negative ? " NEGATIVE-CONTROL" : "", ok, RT_WORDS, untouched, maxErr, guard);
    amdgpu_bo_list_destroy(list); amdgpu_cs_ctx_free(cx);
    return pass ? 0 : 1;
}
