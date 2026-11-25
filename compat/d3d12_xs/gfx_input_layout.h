#pragma once
// Observed slot57 p1 container: count at 0x4c, then length-prefixed semantic
// names and six u32 fields (index, DXGI format, slot, offset, class, step).
// This is a bounded parser of that container, not a shader-signature guess.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

namespace xsx::gfx {
struct InputElement {
    std::string semantic;
    uint32_t index,format,slot,offset,classification,step;
};
inline bool DecodeInputLayout(const void* data,size_t size,std::vector<InputElement>& out)
{
    out.clear();
    if (!data || size<0x50) return false;
    const auto* bytes=static_cast<const uint8_t*>(data);
    auto u32=[&](size_t p) { uint32_t n;std::memcpy(&n,bytes+p,4);return n; };
    const uint32_t count=u32(0x4c);
    if (!count || count>16) return false;
    size_t pos=0x50;
    std::vector<InputElement> decoded;
    for (uint32_t i=0;i<count;++i) {
        if (pos>size || size-pos<4) return false;
        const uint32_t len=u32(pos);pos+=4;
        if (len<2 || len>64 || size-pos<len || size-pos-len<24) return false;
        if (bytes[pos+len-1] || std::memchr(bytes+pos,0,len-1)) return false;
        InputElement e{};e.semantic.assign(reinterpret_cast<const char*>(bytes+pos),len-1);pos+=len;
        e.index=u32(pos);e.format=u32(pos+4);e.slot=u32(pos+8);
        e.offset=u32(pos+12);e.classification=u32(pos+16);e.step=u32(pos+20);pos+=24;
        if (!e.format || e.format>191 || e.slot>=32 || e.offset>2048 ||
            e.classification>1 || (!e.classification && e.step)) return false;
        decoded.push_back(e);
    }
    out=std::move(decoded);return true;
}
}
