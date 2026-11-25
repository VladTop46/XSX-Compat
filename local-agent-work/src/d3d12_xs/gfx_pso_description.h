#pragma once
// Slot57's captured byte layout: opaque qword, then six {address, byte_size}
// records. The prefix is not a size and its interface identity is not needed
// here. Evidence: raw slot57-??-desc.bin, not neighbor-size log extraction.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xsx::gfx {
struct PsoSpan { uint64_t address, size; };
struct PsoDescription {
    uint64_t opaque;
    PsoSpan state, input, ps, psMetadata, vs, vsMetadata;
};
static_assert(sizeof(PsoDescription)==0x68);
static_assert(offsetof(PsoDescription,state)==0x08);
static_assert(offsetof(PsoDescription,input)==0x18);
static_assert(offsetof(PsoDescription,ps)==0x28);
static_assert(offsetof(PsoDescription,vs)==0x48);
static_assert(offsetof(PsoDescription,vsMetadata)==0x58);
inline bool DecodePsoDescription(const void* data,size_t size,PsoDescription& out)
{
    out={};
    if (!data || size<sizeof out) return false;
    PsoDescription decoded{};std::memcpy(&decoded,data,sizeof decoded);
    auto span=[](const PsoSpan& s,uint64_t minimum,uint64_t maximum) {
        return s.address>=0x10000 && s.address<(1ull<<47) &&
            s.size>=minimum && s.size<=maximum && s.size<=(1ull<<47)-s.address;
    };
    // Only this preview's supported VS+PS form. Metadata and the opaque
    // prefix are retained but not dereferenced; no fallback ABI is guessed.
    if (!span(decoded.state,0xf0,4096) || !span(decoded.input,0x50,65536) ||
        !span(decoded.ps,40,1u<<20) || !span(decoded.vs,40,1u<<20)) return false;
    out=decoded;return true;
}
}
