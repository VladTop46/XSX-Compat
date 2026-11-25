// Captured first-three-DWORD V# fixture + explicit synthetic mode boundaries.
#include "../d3d12_xs/gfx_buffer_descriptor.h"
#include <cstdio>
#include <cstdlib>
static unsigned checks=0;
static void require(bool ok){++checks;if(!ok){std::printf("[FAIL] V# check=%u\n",checks);std::exit(1);}}
int main()
{
    using namespace xsx::gfx;
    // game-140716.log line2464 reports these 3 DWORDs. The final DWORD is a
    // synthetic plain-buffer type; no claim is made about its captured value.
    uint32_t words[4]={0xbc92dba0u,0x0010009fu,0x1eu,0};
    BufferDescriptor b{};uint64_t size=0;
    require(DecodeBufferDescriptor(words,b));
    require(b.address==0x9fbc92dba0ull);require(b.stride==16);require(b.records==30);
    require(LinearBufferBytes(b,size) && size==480);
    // Stride0 is a byte range, not an implicit four-byte record.
    words[1]=0x0000009f;words[2]=513;
    require(DecodeBufferDescriptor(words,b) && b.stride==0);
    require(LinearBufferBytes(b,size) && size==513);
    // Cache flag must not leak into stride/address. Swizzle isn't linear.
    words[1]=0xffff009f;words[2]=0xffffffffu;
    require(DecodeBufferDescriptor(words,b));require(b.address==0x9fbc92dba0ull && b.stride==16383);
    require(b.cacheSwizzle && b.swizzle);size=123;
    require(!LinearBufferBytes(b,size) && size==123);
    words[1]=0x0004009f;words[3]=1u<<23;
    require(DecodeBufferDescriptor(words,b) && b.addThreadId);
    require(!LinearBufferBytes(b,size));
    words[3]=0xc0000000u;b.address=42;
    require(!DecodeBufferDescriptor(words,b) && b.address==42);
    words[3]=0;words[2]=0;
    require(DecodeBufferDescriptor(words,b) && !LinearBufferBytes(b,size));
    b={0xfffffffffffeull,4,1,0,false,false,false};
    require(!LinearBufferBytes(b,size));
    std::printf("[PASS] V# descriptor checks=%u captured_address=0x9fbc92dba0 stride=16 records=30 bytes=480\n",checks);
}
