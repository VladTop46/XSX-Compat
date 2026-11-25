// PE side of xsxgpu (builtin-style DLL so Wine loads xsxgpu.so as its unixlib).
#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "xsxgpu_abi.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void* reserved)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(inst); if (__wine_init_unix_call()) return FALSE; }
    return TRUE;
}
int WINAPI xsxgpu_open(unsigned* family, unsigned* device, unsigned long long* vaMax)
{
    struct xsxgpu_open_params p = { 0 };
    NTSTATUS s = WINE_UNIX_CALL(XSXGPU_OPEN, &p);
    if (s) return -100;
    *family = p.family; *device = p.device_id; *vaMax = p.va_max; return p.result;
}
int WINAPI xsxgpu_exp2(void* base, unsigned long long size)
{
    struct xsxgpu_exp2_params p = { (unsigned long long)(ULONG_PTR)base, size, 0 };
    if (WINE_UNIX_CALL(XSXGPU_EXP2, &p)) return -100;
    return p.result;
}
int WINAPI xsxgpu_phys_map(void* va, unsigned long long count, const ULONG_PTR* ids, unsigned long long stride)
{ struct xsxgpu_phys_map_params p = { (ULONG_PTR)va, count, (ULONG_PTR)ids, stride, 0 }; if (WINE_UNIX_CALL(XSXGPU_PHYS_MAP, &p)) return -100; return p.result; }
int WINAPI xsxgpu_phys_unmap(void* va, unsigned long long count)
{ struct xsxgpu_phys_unmap_params p = { (ULONG_PTR)va, count, 0 }; if (WINE_UNIX_CALL(XSXGPU_PHYS_UNMAP, &p)) return -100; return p.result; }
int WINAPI xsxgpu_gpu_fill(void* va, unsigned groups)
{ struct xsxgpu_gpu_fill_params p = { (ULONG_PTR)va, groups, 0 }; if (WINE_UNIX_CALL(XSXGPU_GPU_FILL, &p)) return -100; return p.result; }
void WINAPI xsxgpu_phys_stats(unsigned long long* mapped, unsigned long long* bytes)
{ struct xsxgpu_phys_stats_params p = { 0 }; WINE_UNIX_CALL(XSXGPU_PHYS_STATS, &p); *mapped = p.mapped; *bytes = p.bytes; }
int WINAPI xsxgpu_phys_write(unsigned long long id, unsigned long long offset, const void* src, unsigned long long size)
{ struct xsxgpu_phys_write_params p = { id, offset, (ULONG_PTR)src, size, 0 }; if (WINE_UNIX_CALL(XSXGPU_PHYS_WRITE, &p)) return -100; return p.result; }
