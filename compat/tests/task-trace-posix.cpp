// Actual x64 SIGTRAP execution of the shared instruction-resumption core.
// The Windows probe's handwritten fixture bytes are passed in as a tiny file.
#include "../d3d12_xs/instruction_resume.h"
#include <sys/mman.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

using namespace xsx::diagnostic;
struct Owner { unsigned char pad[0x78]{}; uint32_t generation=0; uint8_t queued=0; unsigned char unused[3]{}; uint32_t pending=0; };
struct Frame { unsigned char pad[0x1548]{}; uint32_t count=0; };
struct Task { unsigned char pad[0x20]{}; Owner* owner; Frame* frame; uint32_t expected; };
using Job = void (__attribute__((ms_abi))*)(Task*);
static constexpr unsigned workers=8, iterations=2000;
static std::array<Point,4> points;
static std::atomic<uint64_t> hits[4]{};
static std::atomic<unsigned> errors{0};
static thread_local uint32_t casFlags=0;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static void Trap(int, siginfo_t*, void* raw)
{
    const int saved=errno;
    auto& g=((ucontext_t*)raw)->uc_mcontext.gregs;
    const auto address=(uintptr_t)g[REG_RIP]-1;
    for (unsigned i=0;i<points.size();++i) if (points[i].address==address) {
        ++hits[i];
        if (i==1) casFlags=(uint32_t)g[REG_EFL]&0x8d5;
        if (i==2 && ((uint32_t)g[REG_EFL]&0x8d5)!=casFlags) ++errors;
        Resume(points[i],g[REG_RIP],g[REG_RAX],g[REG_RCX],g[REG_RSI],(uint32_t)g[REG_EFL]);
        errno=saved; return;
    }
    _exit(2); // Never swallow an unrelated trap.
}
static bool Run(Job job)
{
    std::array<std::thread,workers> threads;
    for (unsigned t=0;t<workers;++t) threads[t]=std::thread([job,t] {
        Owner owner{}; Frame frame{}; Task task{};
        owner.generation=17+t; owner.pending=iterations; task.owner=&owner; task.frame=&frame;
        for (unsigned i=0;i<iterations;++i) {
            const bool accept=!(i&1); const auto before=owner.generation;
            task.expected=before+(accept?0:99); owner.queued=1;
            errno=713;
            job(&task);
            if (errno!=713 || owner.queued || owner.generation!=before+(accept?1:0) ||
                frame.count!=(i+2)/2 || owner.pending!=iterations-i-1) ++errors;
        }
    });
    for (auto& thread:threads) thread.join();
    return errors.load()==0;
}
int main(int argc,char** argv)
{
    if (argc!=2) return 2;
    std::ifstream input(argv[1],std::ios::binary);
    const std::vector<unsigned char> fixture((std::istreambuf_iterator<char>(input)),{});
    if (fixture.size()!=128) return 2;
    auto* code=(unsigned char*)mmap(nullptr,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (code==MAP_FAILED) return 2;
    std::memcpy(code,fixture.data(),fixture.size());
    const auto base=(uintptr_t)code;
    points={{{base+5,Instruction::RaxFromRcx},{base+27,Instruction::JnzPlusFive},
        {base+65,Instruction::RsiFromRcx},{base+76,Instruction::RcxFromRsi}}};
    const auto job=(Job)code;
    if (!Run(job)) { std::puts("[FAIL] native fixture"); return 1; }
    struct sigaction action{},previous{}; action.sa_sigaction=Trap; action.sa_flags=SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP,&action,&previous)) return 2;
    for (const auto& point:points) *(volatile unsigned char*)point.address=0xcc;
    Run(job);
    bool passed=errors.load()==0;
    for (unsigned i=0;i<4;++i) passed=passed && hits[i].load()==uint64_t(workers)*iterations/(i<2?1:2);
    for (const auto& point:points) *(volatile unsigned char*)point.address=fixture[point.address-base];
    sigaction(SIGTRAP,&previous,nullptr);
    passed=passed && !std::memcmp(code,fixture.data(),fixture.size());
    munmap(code,4096);
    std::printf("[%s] native SIGTRAP calls=%u entry=%llu post_cas=%llu update=%llu after_increment=%llu errors=%u\n",
        passed?"PASS":"FAIL",workers*iterations,(unsigned long long)hits[0].load(),(unsigned long long)hits[1].load(),
        (unsigned long long)hits[2].load(),(unsigned long long)hits[3].load(),errors.load());
    return passed?0:1;
}
