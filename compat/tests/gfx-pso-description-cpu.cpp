// Native CPU regression using raw captures, independent byte offsets and
// deliberately truncated allocations. No Wine/GPU/fixture warmup required.
#include "../../local-agent-work/src/d3d12_xs/gfx_pso_description.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
static unsigned checks=0;
static void check(bool ok,const char* why) {
    ++checks;if (!ok) { std::fprintf(stderr,"[FAIL] %s\n",why);std::exit(1); }
}
static void put(std::vector<uint8_t>& data,size_t offset,uint64_t value) { std::memcpy(data.data()+offset,&value,8); }
int main(int argc,char** argv) {
    using xsx::gfx::PsoDescription;using xsx::gfx::DecodePsoDescription;
    check(argc==2,"capture directory argument");
    std::vector<uint8_t> raw(104);
    const std::array<uint64_t,6> lengths={388,188,384,112,320,48};
    put(raw,0,0xfedcba9876543210ull);
    for (unsigned i=0;i<6;++i) { put(raw,8+16*i,0x1234567000ull+0x1000*i);put(raw,16+16*i,lengths[i]); }
    PsoDescription d{};check(DecodePsoDescription(raw.data(),raw.size(),d),"104-byte pointer-first header");
    check(d.opaque==0xfedcba9876543210ull,"opaque prefix retained without pointer/size validation");
    check(d.state.address==0x1234567000ull && d.state.size==388,"state offsets8/16");
    check(d.input.address==0x1234568000ull && d.input.size==188,"input offsets24/32");
    check(d.ps.address==0x1234569000ull && d.ps.size==384,"PS offsets40/48");
    check(d.psMetadata.address==0x123456a000ull && d.psMetadata.size==112,"PS metadata offsets56/64");
    check(d.vs.address==0x123456b000ull && d.vs.size==320,"VS offsets72/80");
    check(d.vsMetadata.address==0x123456c000ull && d.vsMetadata.size==48,"VS metadata offsets88/96");
    for (size_t n=0;n<raw.size();++n) {
        std::vector<uint8_t> prefix(raw.begin(),raw.begin()+n);
        check(!DecodePsoDescription(prefix.data(),prefix.size(),d),"reject incomplete allocation");
        check(!d.opaque && !d.state.address,"no partial output on failure");
    }
    check(!DecodePsoDescription(nullptr,104,d),"null header");
    for (uint64_t opaque:{0ull,1ull,4096ull,0xd51125c010ull,~0ull}) {
        auto changed=raw;put(changed,0,opaque);
        check(DecodePsoDescription(changed.data(),changed.size(),d) && d.opaque==opaque,"prefix never used as span size");
    }
    for (size_t offset:{8u,24u,40u,72u}) for (uint64_t pointer:{0ull,1ull,0x800000000000ull,~0ull}) {
        auto changed=raw;put(changed,offset,pointer);
        check(!DecodePsoDescription(changed.data(),changed.size(),d),"invalid span address");
    }
    const unsigned offsets[]={16,32,48,80};const uint64_t mins[]={240,80,40,40},maxs[]={4096,65536,1048576,1048576};
    for (unsigned i=0;i<4;++i) for (uint64_t length:{mins[i]-1,maxs[i]+1,UINT64_MAX}) {
        auto changed=raw;put(changed,offsets[i],length);
        check(!DecodePsoDescription(changed.data(),changed.size(),d),"invalid declared extent");
    }
    auto old=raw;std::fill(old.begin(),old.end(),0);
    for (unsigned i=0;i<6;++i) { put(old,16*i,lengths[i]);put(old,8+16*i,0x1234567000ull+0x1000*i); }
    check(!DecodePsoDescription(old.data(),old.size(),d),"old synthetic size-first ABI rejected");
    // These eight unmodified captures predate the candidate. Lengths below
    // come from the independently saved pair files, not from struct members.
    const uint64_t captured[8][6]={
        {388,188,512,208,512,176},{388,188,448,208,384,752},
        {380,188,0,0,320,48},{388,188,768,208,320,752},
        {388,188,448,208,384,752},{388,188,384,112,320,48},
        {380,120,0,0,384,112},{388,188,448,208,384,752}};
    unsigned supported=0;
    for (unsigned i=0;i<8;++i) {
        char filename[64];std::snprintf(filename,sizeof filename,"slot57-%02u-desc.bin",i);
        std::ifstream file(std::string(argv[1])+"/"+filename,std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
        check(bytes.size()==256,"complete actual capture");
        const bool ok=DecodePsoDescription(bytes.data(),bytes.size(),d);
        check(ok==(i!=2 && i!=6),"actual VS+PS accepted, missing PS rejected");
        if (!ok) continue;
        ++supported;
        const xsx::gfx::PsoSpan spans[]={d.state,d.input,d.ps,d.psMetadata,d.vs,d.vsMetadata};
        for (unsigned p=0;p<6;++p) {
            check(spans[p].size==captured[i][p],"actual independently saved extent");
            char part[64];std::snprintf(part,sizeof part,"slot57-%02u-pair%02u.bin",i,p);
            std::ifstream blob(std::string(argv[1])+"/"+part,std::ios::binary|std::ios::ate);
            check(blob && (uint64_t)blob.tellg()==spans[p].size,"extent equals actual blob file length");
        }
    }
    std::printf("[PASS] native slot57 description checks=%u captures=8 supported=%u missing_ps_rejected=2 truncated_prefixes=104\n",checks,supported);
}
