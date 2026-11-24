// СГЕНЕРИРОВАНО (compat/tools/gen-fwd.py). Smart-forward: Xbox-ABI авто-детект (RCX=ret, RDX=this) -> PC-ABI; this=wrapper -> real.
extern "C" { extern uintptr_t g_arena_lo, g_arena_hi;
void xs_fwd_0();
void xs_fwd_1();
void xs_fwd_2();
void xs_fwd_3();
void xs_fwd_4();
void xs_fwd_5();
void xs_fwd_6();
void xs_fwd_7();
void xs_fwd_8();
void xs_fwd_9();
void xs_fwd_10();
void xs_fwd_11();
void xs_fwd_12();
void xs_fwd_13();
void xs_fwd_14();
void xs_fwd_15();
void xs_fwd_16();
void xs_fwd_17();
void xs_fwd_18();
void xs_fwd_19();
void xs_fwd_20();
void xs_fwd_21();
void xs_fwd_22();
void xs_fwd_23();
void xs_fwd_24();
void xs_fwd_25();
void xs_fwd_26();
void xs_fwd_27();
void xs_fwd_28();
void xs_fwd_29();
void xs_fwd_30();
void xs_fwd_31();
void xs_fwd_32();
void xs_fwd_33();
void xs_fwd_34();
void xs_fwd_35();
void xs_fwd_36();
void xs_fwd_37();
void xs_fwd_38();
void xs_fwd_39();
void xs_fwd_40();
void xs_fwd_41();
void xs_fwd_42();
void xs_fwd_43();
void xs_fwd_44();
void xs_fwd_45();
void xs_fwd_46();
void xs_fwd_47();
void xs_fwd_48();
void xs_fwd_49();
void xs_fwd_50();
void xs_fwd_51();
void xs_fwd_52();
void xs_fwd_53();
void xs_fwd_54();
void xs_fwd_55();
void xs_fwd_56();
void xs_fwd_57();
void xs_fwd_58();
void xs_fwd_59();
void xs_fwd_60();
void xs_fwd_61();
void xs_fwd_62();
void xs_fwd_63();
void xs_fwd_64();
void xs_fwd_65();
void xs_fwd_66();
void xs_fwd_67();
void xs_fwd_68();
void xs_fwd_69();
void xs_fwd_70();
void xs_fwd_71();
void xs_fwd_72();
void xs_fwd_73();
void xs_fwd_74();
void xs_fwd_75();
void xs_fwd_76();
void xs_fwd_77();
void xs_fwd_78();
void xs_fwd_79();
}
asm(R"ASM(
.text
.globl xs_fwd_0
.p2align 4
xs_fwd_0:
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
    jmpq *0(%rax)
.globl xs_fwd_1
.p2align 4
xs_fwd_1:
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
    jmpq *8(%rax)
.globl xs_fwd_2
.p2align 4
xs_fwd_2:
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
    jmpq *16(%rax)
.globl xs_fwd_3
.p2align 4
xs_fwd_3:
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
    jmpq *24(%rax)
.globl xs_fwd_4
.p2align 4
xs_fwd_4:
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
    jmpq *32(%rax)
.globl xs_fwd_5
.p2align 4
xs_fwd_5:
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
    jmpq *40(%rax)
.globl xs_fwd_6
.p2align 4
xs_fwd_6:
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
    jmpq *48(%rax)
.globl xs_fwd_7
.p2align 4
xs_fwd_7:
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
    jmpq *56(%rax)
.globl xs_fwd_8
.p2align 4
xs_fwd_8:
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
    jmpq *64(%rax)
.globl xs_fwd_9
.p2align 4
xs_fwd_9:
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
    jmpq *72(%rax)
.globl xs_fwd_10
.p2align 4
xs_fwd_10:
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
    jmpq *80(%rax)
.globl xs_fwd_11
.p2align 4
xs_fwd_11:
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
    jmpq *88(%rax)
.globl xs_fwd_12
.p2align 4
xs_fwd_12:
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
    jmpq *96(%rax)
.globl xs_fwd_13
.p2align 4
xs_fwd_13:
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
    jmpq *104(%rax)
.globl xs_fwd_14
.p2align 4
xs_fwd_14:
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
    jmpq *112(%rax)
.globl xs_fwd_15
.p2align 4
xs_fwd_15:
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
    jmpq *120(%rax)
.globl xs_fwd_16
.p2align 4
xs_fwd_16:
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
    jmpq *128(%rax)
.globl xs_fwd_17
.p2align 4
xs_fwd_17:
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
    jmpq *136(%rax)
.globl xs_fwd_18
.p2align 4
xs_fwd_18:
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
    jmpq *144(%rax)
.globl xs_fwd_19
.p2align 4
xs_fwd_19:
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
    jmpq *152(%rax)
.globl xs_fwd_20
.p2align 4
xs_fwd_20:
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
    jmpq *160(%rax)
.globl xs_fwd_21
.p2align 4
xs_fwd_21:
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
    jmpq *168(%rax)
.globl xs_fwd_22
.p2align 4
xs_fwd_22:
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
    jmpq *176(%rax)
.globl xs_fwd_23
.p2align 4
xs_fwd_23:
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
    jmpq *184(%rax)
.globl xs_fwd_24
.p2align 4
xs_fwd_24:
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
    jmpq *192(%rax)
.globl xs_fwd_25
.p2align 4
xs_fwd_25:
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
    jmpq *200(%rax)
.globl xs_fwd_26
.p2align 4
xs_fwd_26:
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
    jmpq *208(%rax)
.globl xs_fwd_27
.p2align 4
xs_fwd_27:
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
    jmpq *216(%rax)
.globl xs_fwd_28
.p2align 4
xs_fwd_28:
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
    jmpq *224(%rax)
.globl xs_fwd_29
.p2align 4
xs_fwd_29:
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
    jmpq *232(%rax)
.globl xs_fwd_30
.p2align 4
xs_fwd_30:
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
    jmpq *240(%rax)
.globl xs_fwd_31
.p2align 4
xs_fwd_31:
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
    jmpq *248(%rax)
.globl xs_fwd_32
.p2align 4
xs_fwd_32:
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
    jmpq *256(%rax)
.globl xs_fwd_33
.p2align 4
xs_fwd_33:
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
    jmpq *264(%rax)
.globl xs_fwd_34
.p2align 4
xs_fwd_34:
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
    jmpq *272(%rax)
.globl xs_fwd_35
.p2align 4
xs_fwd_35:
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
    jmpq *280(%rax)
.globl xs_fwd_36
.p2align 4
xs_fwd_36:
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
    jmpq *288(%rax)
.globl xs_fwd_37
.p2align 4
xs_fwd_37:
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
    jmpq *296(%rax)
.globl xs_fwd_38
.p2align 4
xs_fwd_38:
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
    jmpq *304(%rax)
.globl xs_fwd_39
.p2align 4
xs_fwd_39:
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
    jmpq *312(%rax)
.globl xs_fwd_40
.p2align 4
xs_fwd_40:
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
    jmpq *320(%rax)
.globl xs_fwd_41
.p2align 4
xs_fwd_41:
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
    jmpq *328(%rax)
.globl xs_fwd_42
.p2align 4
xs_fwd_42:
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
    jmpq *336(%rax)
.globl xs_fwd_43
.p2align 4
xs_fwd_43:
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
    jmpq *344(%rax)
.globl xs_fwd_44
.p2align 4
xs_fwd_44:
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
    jmpq *352(%rax)
.globl xs_fwd_45
.p2align 4
xs_fwd_45:
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
    jmpq *360(%rax)
.globl xs_fwd_46
.p2align 4
xs_fwd_46:
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
    jmpq *368(%rax)
.globl xs_fwd_47
.p2align 4
xs_fwd_47:
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
    jmpq *376(%rax)
.globl xs_fwd_48
.p2align 4
xs_fwd_48:
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
    jmpq *384(%rax)
.globl xs_fwd_49
.p2align 4
xs_fwd_49:
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
    jmpq *392(%rax)
.globl xs_fwd_50
.p2align 4
xs_fwd_50:
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
    jmpq *400(%rax)
.globl xs_fwd_51
.p2align 4
xs_fwd_51:
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
    jmpq *408(%rax)
.globl xs_fwd_52
.p2align 4
xs_fwd_52:
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
    jmpq *416(%rax)
.globl xs_fwd_53
.p2align 4
xs_fwd_53:
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
    jmpq *424(%rax)
.globl xs_fwd_54
.p2align 4
xs_fwd_54:
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
    jmpq *432(%rax)
.globl xs_fwd_55
.p2align 4
xs_fwd_55:
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
    jmpq *440(%rax)
.globl xs_fwd_56
.p2align 4
xs_fwd_56:
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
    jmpq *448(%rax)
.globl xs_fwd_57
.p2align 4
xs_fwd_57:
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
    jmpq *456(%rax)
.globl xs_fwd_58
.p2align 4
xs_fwd_58:
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
    jmpq *464(%rax)
.globl xs_fwd_59
.p2align 4
xs_fwd_59:
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
    jmpq *472(%rax)
.globl xs_fwd_60
.p2align 4
xs_fwd_60:
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
    jmpq *480(%rax)
.globl xs_fwd_61
.p2align 4
xs_fwd_61:
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
    jmpq *488(%rax)
.globl xs_fwd_62
.p2align 4
xs_fwd_62:
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
    jmpq *496(%rax)
.globl xs_fwd_63
.p2align 4
xs_fwd_63:
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
    jmpq *504(%rax)
.globl xs_fwd_64
.p2align 4
xs_fwd_64:
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
    jmpq *512(%rax)
.globl xs_fwd_65
.p2align 4
xs_fwd_65:
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
    jmpq *520(%rax)
.globl xs_fwd_66
.p2align 4
xs_fwd_66:
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
    jmpq *528(%rax)
.globl xs_fwd_67
.p2align 4
xs_fwd_67:
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
    jmpq *536(%rax)
.globl xs_fwd_68
.p2align 4
xs_fwd_68:
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
    jmpq *544(%rax)
.globl xs_fwd_69
.p2align 4
xs_fwd_69:
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
    jmpq *552(%rax)
.globl xs_fwd_70
.p2align 4
xs_fwd_70:
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
    jmpq *560(%rax)
.globl xs_fwd_71
.p2align 4
xs_fwd_71:
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
    jmpq *568(%rax)
.globl xs_fwd_72
.p2align 4
xs_fwd_72:
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
    jmpq *576(%rax)
.globl xs_fwd_73
.p2align 4
xs_fwd_73:
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
    jmpq *584(%rax)
.globl xs_fwd_74
.p2align 4
xs_fwd_74:
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
    jmpq *592(%rax)
.globl xs_fwd_75
.p2align 4
xs_fwd_75:
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
    jmpq *600(%rax)
.globl xs_fwd_76
.p2align 4
xs_fwd_76:
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
    jmpq *608(%rax)
.globl xs_fwd_77
.p2align 4
xs_fwd_77:
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
    jmpq *616(%rax)
.globl xs_fwd_78
.p2align 4
xs_fwd_78:
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
    jmpq *624(%rax)
.globl xs_fwd_79
.p2align 4
xs_fwd_79:
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
    jmpq *632(%rax)
)ASM");
