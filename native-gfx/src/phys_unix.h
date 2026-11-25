#pragma once
#include <stdint.h>
#include <amdgpu.h>
void phys_init(int fd, amdgpu_device_handle dev);
int phys_map(uint64_t va, uint64_t count, const uint64_t* ids, uint64_t id_stride);
int phys_unmap(uint64_t va, uint64_t count);
void phys_stats(uint64_t* mapped, uint64_t* bytes);
unsigned phys_bo_list(amdgpu_bo_handle* out, unsigned max);
int phys_write(uint64_t id, uint64_t offset, const void* src, uint64_t size);
