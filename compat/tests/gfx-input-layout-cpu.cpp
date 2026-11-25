// Optional private fixtures are external files; this source embeds no game data.
#include "../d3d12_xs/gfx_input_layout.h"
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
static unsigned checks=0;
static void check(bool ok) { ++checks;if (!ok) { std::printf("[FAIL] input-layout check=%u\n",checks);std::exit(1); } }
int main(int argc,char** argv)
{
    check(argc==2);
    for (unsigned i=1;i<=4;++i) {
        std::ifstream input(std::string(argv[1])+"/pso"+std::to_string(i)+"_q3.bin",std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        std::vector<xsx::gfx::InputElement> elements;
        check(xsx::gfx::DecodeInputLayout(bytes.data(),bytes.size(),elements));check(elements.size()==3);
        if (i<=2) {
            check(elements[0].semantic=="POSITION" && elements[0].format==6 && elements[0].offset==0);
            check(elements[1].semantic=="COLOR" && elements[1].format==28 && elements[1].offset==12);
            check(elements[2].semantic=="TEXCOORD" && elements[2].format==16 && elements[2].offset==16);
        } else {
            check(elements[0].semantic=="COLOR" && elements[0].index==0 && elements[0].format==28 && elements[0].offset==0);
            check(elements[1].semantic=="COLOR" && elements[1].index==1 && elements[1].format==28 && elements[1].offset==4);
            check(elements[2].semantic=="POSITION" && elements[2].format==16 && elements[2].offset==8);
        }
        const size_t end=0x50+elements[0].semantic.size()+1+4+24;
        check(!xsx::gfx::DecodeInputLayout(bytes.data(),end-1,elements) && elements.empty());
        const size_t layoutEnd=i<=2?188:185;
        for (size_t prefix=0;prefix<layoutEnd;++prefix)
            check(!xsx::gfx::DecodeInputLayout(bytes.data(),prefix,elements) && elements.empty());
        auto invalid=bytes;invalid[0x4c]=255;
        check(!xsx::gfx::DecodeInputLayout(invalid.data(),invalid.size(),elements) && elements.empty());
        invalid=bytes;invalid[0x50]=0;
        check(!xsx::gfx::DecodeInputLayout(invalid.data(),invalid.size(),elements));
        invalid=bytes;invalid[0x54]=0;
        check(!xsx::gfx::DecodeInputLayout(invalid.data(),invalid.size(),elements));
    }
    std::vector<xsx::gfx::InputElement> elements;
    check(!xsx::gfx::DecodeInputLayout(nullptr,4096,elements));
    std::printf("[PASS] input layouts checks=%u fixtures=4 packed_RGBA8=1 truncated_rejected=1\n",checks);
}
