// Unix side of xsxgpu: runs inside the Wine process, talks to amdgpu directly.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "xsxgpu_abi.h"
#include "phys_unix.h"
#include "gpu_unix.h"
#include <stdlib.h>
int xsx_exp2_draw(amdgpu_device_handle dev, uint8_t* m, uint64_t SIZE, const char* dir, int argc, char** argv);

static int g_fd = -1; static amdgpu_device_handle g_dev;

static NTSTATUS gpu_open(void* args)
{
    struct xsxgpu_open_params* p = args;
    if (g_fd < 0) {
        g_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (g_fd < 0) { p->result = -1; return STATUS_SUCCESS; }
        uint32_t ma, mi;
        if (amdgpu_device_initialize(g_fd, &ma, &mi, &g_dev)) { p->result = -2; return STATUS_SUCCESS; }
        phys_init(g_fd, g_dev);
    }
    struct amdgpu_gpu_info gi; amdgpu_query_gpu_info(g_dev, &gi);
    struct drm_amdgpu_info_device di; amdgpu_query_info(g_dev, AMDGPU_INFO_DEV_INFO, sizeof di, &di);
    p->family = gi.family_id; p->device_id = di.device_id; p->va_max = di.virtual_address_max; p->result = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS gpu_exp2(void* args)
{
    struct xsxgpu_exp2_params* p = args;
    const char* dir = getenv("XSXGPU_DATA");
    if (g_fd < 0 || !dir) { p->result = -1; return STATUS_SUCCESS; }
    p->result = xsx_exp2_draw(g_dev, (uint8_t*)(uintptr_t)p->base, p->size, dir, 1, NULL);
    fflush(stdout);
    return STATUS_SUCCESS;
}
static NTSTATUS u_phys_map(void* a) { struct xsxgpu_phys_map_params* p = a; p->result = g_fd < 0 ? -1 : phys_map(p->va, p->count, (const uint64_t*)(uintptr_t)p->ids, p->stride); return STATUS_SUCCESS; }
static NTSTATUS u_phys_unmap(void* a) { struct xsxgpu_phys_unmap_params* p = a; p->result = g_fd < 0 ? -1 : phys_unmap(p->va, p->count); return STATUS_SUCCESS; }
static NTSTATUS u_gpu_fill(void* a) { struct xsxgpu_gpu_fill_params* p = a; p->result = g_fd < 0 ? -1 : gpu_fill(g_dev, p->va, p->groups); return STATUS_SUCCESS; }
static NTSTATUS u_phys_stats(void* a) { struct xsxgpu_phys_stats_params* p = a; phys_stats((uint64_t*)&p->mapped, (uint64_t*)&p->bytes); return STATUS_SUCCESS; }
static NTSTATUS u_phys_write(void* a) { struct xsxgpu_phys_write_params* p = a; p->result = g_fd < 0 ? -1 : phys_write(p->id, p->offset, (const void*)(uintptr_t)p->src, p->size); return STATUS_SUCCESS; }
const unixlib_entry_t __wine_unix_call_funcs[] = { gpu_open, gpu_exp2, u_phys_map, u_phys_unmap, u_gpu_fill, u_phys_stats, u_phys_write };
