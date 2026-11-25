#pragma once
// GFX10 buffer SRD fields, independently decoded from AMD's public layout:
// GPUOpen PAL chip/gfx10_sq_ko_reg.h, sq_buf_rsrc_t. This is NOT an image T#
// decoder and does not turn a guest address into a native GPU address.
#include <cstdint>

namespace xsx::gfx {
struct BufferDescriptor {
    uint64_t address;
    uint32_t stride,records,format;
    bool cacheSwizzle,swizzle,addThreadId;
};
inline bool DecodeBufferDescriptor(const uint32_t words[4],BufferDescriptor& out)
{
    if ((words[3]>>30)!=0) return false; // Buffer type only.
    out={uint64_t(words[0]) | (uint64_t(words[1]&0xffffu)<<32),
         (words[1]>>16)&0x3fffu,words[2],(words[3]>>12)&0x7fu,
         bool(words[1]&(1u<<30)),bool(words[1]&(1u<<31)),bool(words[3]&(1u<<23))};
    return true;
}
// Extent for the explicit linear byte/record model. Other address modes need
// their own lowering; never guess a length such as 4096 records for them.
inline bool LinearBufferBytes(const BufferDescriptor& descriptor,uint64_t& bytes)
{
    if (descriptor.swizzle || descriptor.addThreadId || !descriptor.records) return false;
    const uint64_t size=uint64_t(descriptor.records)*(descriptor.stride?descriptor.stride:1u);
    if (descriptor.address>=(1ull<<48) || size>(1ull<<48)-descriptor.address) return false;
    bytes=size;return true;
}
}
