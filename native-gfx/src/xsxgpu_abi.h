#pragma once
enum xsxgpu_funcs { XSXGPU_OPEN, XSXGPU_EXP2, XSXGPU_PHYS_MAP, XSXGPU_PHYS_UNMAP, XSXGPU_GPU_FILL, XSXGPU_PHYS_STATS, XSXGPU_PHYS_WRITE };
struct xsxgpu_open_params { int result; unsigned family, device_id; unsigned long long va_max; };
struct xsxgpu_exp2_params { unsigned long long base, size; int result; };
struct xsxgpu_phys_map_params { unsigned long long va, count, ids, stride; int result; };
struct xsxgpu_phys_unmap_params { unsigned long long va, count; int result; };
struct xsxgpu_gpu_fill_params { unsigned long long va; unsigned groups; int result; };
struct xsxgpu_phys_stats_params { unsigned long long mapped, bytes; };
struct xsxgpu_phys_write_params { unsigned long long id, offset, src, size; int result; };
