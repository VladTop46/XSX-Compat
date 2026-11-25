#pragma once
#include <cstdint>
namespace xsx::diagnostic {
enum class Instruction { RaxFromRcx, RsiFromRcx, RcxFromRsi, JnzPlusFive };
struct Point { uintptr_t address; Instruction instruction; };
// Only the named register and RIP change; arithmetic flags, stack and memory
// are untouched. Both native CPU signal and Windows VEH probes use this core.
template<class Word> inline void Resume(Point point, Word& rip, Word& rax,
    Word& rcx, Word& rsi, uint32_t flags)
{
    switch (point.instruction) {
    case Instruction::RaxFromRcx: rax = rcx; rip = point.address + 3; break;
    case Instruction::RsiFromRcx: rsi = rcx; rip = point.address + 3; break;
    case Instruction::RcxFromRsi: rcx = rsi; rip = point.address + 3; break;
    case Instruction::JnzPlusFive: rip = point.address + ((flags & 0x40) ? 2 : 7); break;
    }
}
} // namespace xsx::diagnostic
