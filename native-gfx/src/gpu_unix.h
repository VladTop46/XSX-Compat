#pragma once
#include <stdint.h>
#include <amdgpu.h>
int gpu_fill(amdgpu_device_handle dev, uint64_t out_va, unsigned groups);
