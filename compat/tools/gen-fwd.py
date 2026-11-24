#!/usr/bin/env python3 -I
"""Генерирует compat/d3d12_xs/d3d12_xs_fwd.inc: asm-переходники xs_fwd_N (N=0..43) для device-обёртки. См. d3d12_xs.cpp."""
import sys
N = int(sys.argv[1]) if len(sys.argv) > 1 else 44
out = ['// СГЕНЕРИРОВАНО (compat/tools/gen-fwd.py). Smart-forward: Xbox-ABI авто-детект (RCX=ret, RDX=this) -> PC-ABI; this=wrapper -> real.',
       'extern "C" { extern uintptr_t g_arena_lo, g_arena_hi;']
out += [f'void xs_fwd_{i}();' for i in range(N)]
out.append('}')
asm = '.text\n'
for i in range(N):
    asm += f'''.globl xs_fwd_{i}
.p2align 4
xs_fwd_{i}:
    movq g_arena_lo(%rip), %rax
    cmpq %rax, %rdx
    jb 1f
    cmpq g_arena_hi(%rip), %rdx
    jae 1f
    cmpq %rax, %rcx
    jb 2f
    cmpq g_arena_hi(%rip), %rcx
    jae 2f
    jmp 1f
2:  xchgq %rcx, %rdx
1:  movq 8(%rcx), %rcx
    movq (%rcx), %rax
    jmpq *{i*8}(%rax)
'''
out.append('asm(R"ASM(\n' + asm + ')ASM");')
print('\n'.join(out))
