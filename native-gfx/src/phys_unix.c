// M2: Xbox physical pages backed by amdgpu GTT BOs (unix side of xsxgpu).
// Page id (XMem: multiple of 16, first 0x100000) -> chunk BO + offset. A mapping places the
// same 64 KiB of the BO at a guest VA for the CPU (mmap of the DRM fd, MAP_FIXED over the
// Wine reservation) AND at the same GPU VA. Aliases = several mappings of one page.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <xf86drm.h>
#include "phys_unix.h"

#define PAGE 0x10000ull
#define CHUNK_PAGES 512ull              /* 32 MiB per BO */
#define MAX_CHUNKS 1024                 /* 32 GiB of ids */
#define FIRST_ID 0x100000ull
#define ID_STEP 16ull
struct chunk { amdgpu_bo_handle bo; int dmabuf; uint8_t* cpu; };
static struct chunk g_chunks[MAX_CHUNKS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1; static amdgpu_device_handle g_dev;
static uint64_t g_mapped_pages, g_bytes_alloc;

void phys_init(int fd, amdgpu_device_handle dev) { g_fd = fd; g_dev = dev; }

static int page_of(uint64_t id, struct chunk** c, uint64_t* off)
{
    if (id < FIRST_ID || (id - FIRST_ID) % ID_STEP) return -EINVAL;
    const uint64_t n = (id - FIRST_ID) / ID_STEP, ci = n / CHUNK_PAGES;
    if (ci >= MAX_CHUNKS) return -ERANGE;
    struct chunk* ch = &g_chunks[ci];
    if (!ch->bo) {
        struct amdgpu_bo_alloc_request req = { .alloc_size = CHUNK_PAGES * PAGE, .phys_alignment = PAGE, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT };
        if (amdgpu_bo_alloc(g_dev, &req, &ch->bo)) return -ENOMEM;
        // dma-buf mmap accepts any page offset inside the buffer (DRM GEM mmap does not).
        uint32_t fd; if (amdgpu_bo_export(ch->bo, amdgpu_bo_handle_type_dma_buf_fd, &fd)) return -EIO;
        ch->dmabuf = (int)fd; g_bytes_alloc += CHUNK_PAGES * PAGE;
    }
    *c = ch; *off = (n % CHUNK_PAGES) * PAGE; return 0;
}

int phys_map(uint64_t va, uint64_t count, const uint64_t* ids, uint64_t id_stride_pages)
{
    if (va % PAGE) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    int r = 0;
    for (uint64_t i = 0; i < count && !r; i++) {
        const uint64_t id = ids ? ids[i * id_stride_pages] : 0, a = va + i * PAGE;
        struct chunk* c; uint64_t off;
        if ((r = page_of(id, &c, &off))) break;
        amdgpu_bo_va_op_raw(g_dev, NULL, 0, PAGE, a, 0, AMDGPU_VA_OP_CLEAR);
        if (mmap((void*)(uintptr_t)a, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, c->dmabuf, off) != (void*)(uintptr_t)a) { r = -errno; break; }
        if (amdgpu_bo_va_op_raw(g_dev, c->bo, off, PAGE, a, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP)) { r = -EFAULT; break; }
        g_mapped_pages++;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

int phys_unmap(uint64_t va, uint64_t count)
{
    pthread_mutex_lock(&g_lock);
    amdgpu_bo_va_op_raw(g_dev, NULL, 0, count * PAGE, va, 0, AMDGPU_VA_OP_CLEAR);
    // Back to an inaccessible reservation, like an unmapped placeholder.
    void* p = mmap((void*)(uintptr_t)va, count * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    pthread_mutex_unlock(&g_lock);
    return p == (void*)(uintptr_t)va ? 0 : -errno;
}

void phys_stats(uint64_t* mapped, uint64_t* bytes) { *mapped = g_mapped_pages; *bytes = g_bytes_alloc; }

// Chunk BOs are ordinary per-process BOs: every submission must list them.
unsigned phys_bo_list(amdgpu_bo_handle* out, unsigned max)
{
    unsigned n = 0; pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < MAX_CHUNKS && n < max; i++) if (g_chunks[i].bo) out[n++] = g_chunks[i].bo;
    pthread_mutex_unlock(&g_lock); return n;
}

// CPU write into a physical page by id (DirectStorage destinations given as page lists).
int phys_write(uint64_t id, uint64_t offset, const void* src, uint64_t size)
{
    if (offset + size > PAGE) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    struct chunk* c; uint64_t off; int r = page_of(id, &c, &off);
    if (!r && !c->cpu) {
        void* p = mmap(NULL, CHUNK_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, c->dmabuf, 0);
        if (p == MAP_FAILED) r = -errno; else c->cpu = p;
    }
    pthread_mutex_unlock(&g_lock);
    if (r) return r;
    memcpy(c->cpu + off + offset, src, size);
    return 0;
}
