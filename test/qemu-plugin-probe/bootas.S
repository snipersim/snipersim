/*
 * Two address spaces with different code and data at one virtual address,
 * for the full-system trace (testing/test_qemu.py, system-address-spaces).
 *
 * Boots like boot64.S, then builds two sets of page tables that agree on the
 * identity-mapped first GiB and differ on one page:
 *
 *   VA 0x40000000 -> PA 0x200000 in A,  PA 0x400000 in B   (2 MiB pages)
 *
 * It copies different code into each physical page -- A returns 1, B
 * returns 2, and their first instructions differ in bytes and length -- and
 * each stores its own value at VA 0x40000800.  Then it switches CR3 between
 * the two and calls VA 0x40000000, NROUNDS times each, as two processes
 * would run one after the other.
 *
 * Exit (isa-debug-exit) 0x10 when the calls returned NROUNDS * 3 in total
 * and each process's store landed in its own physical page; 0x20 otherwise.
 * A trace of this run must carry A's bytes and B's bytes alternately at the
 * one address, and the stores at two physical addresses.
 */
    .set PML4_A, 0x10000
    .set PDPT_A, 0x11000
    .set PD0,    0x12000         /* identity, first 1 GiB, shared */
    .set PD1_A,  0x13000
    .set PML4_B, 0x14000
    .set PDPT_B, 0x15000
    .set PD1_B,  0x16000

    .set PRESENT_RW,      0x003
    .set PRESENT_RW_HUGE, 0x083

    .set CODE_VA,  0x40000000
    .set DATA_VA,  0x40000800
    .set PHYS_A,   0x200000
    .set PHYS_B,   0x400000
    .set NROUNDS,  4

    .code16
    .section .text
    .globl _start
_start:
    cli
    xor  %ax, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  $0x7c00, %sp

    /* zero the seven table pages */
    mov  $PML4_A, %ebx
    xor  %eax, %eax
    mov  $(7 * 4096 / 4), %ecx
1:  movl %eax, (%ebx)
    add  $4, %ebx
    dec  %ecx
    jnz  1b

    mov  $PML4_A, %edi
    movl $(PDPT_A | PRESENT_RW), (%edi)
    mov  $PML4_B, %edi
    movl $(PDPT_B | PRESENT_RW), (%edi)
    mov  $PDPT_A, %edi
    movl $(PD0 | PRESENT_RW), (%edi)
    movl $(PD1_A | PRESENT_RW), 8(%edi)
    mov  $PDPT_B, %edi
    movl $(PD0 | PRESENT_RW), (%edi)
    movl $(PD1_B | PRESENT_RW), 8(%edi)

    mov  $PD0, %ebx
    xor  %eax, %eax
    mov  $512, %ecx
2:  mov  %eax, %edx
    or   $PRESENT_RW_HUGE, %edx
    movl %edx, (%ebx)
    add  $0x200000, %eax
    add  $8, %ebx
    dec  %ecx
    jnz  2b

    mov  $PD1_A, %edi
    movl $(PHYS_A | PRESENT_RW_HUGE), (%edi)
    mov  $PD1_B, %edi
    movl $(PHYS_B | PRESENT_RW_HUGE), (%edi)

    mov  %cr4, %eax
    or   $(1 << 5), %eax
    mov  %eax, %cr4
    mov  $PML4_A, %eax
    mov  %eax, %cr3
    mov  $0xC0000080, %ecx
    rdmsr
    or   $(1 << 8), %eax
    wrmsr
    mov  %cr0, %eax
    or   $((1 << 31) | 1), %eax
    mov  %eax, %cr0
    lgdt gdt_desc
    ljmp $0x08, $long_mode

    .code64
long_mode:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  $0x7c00, %rsp

    /* each process's code into its physical page (identity mapped here) */
    lea  code_a(%rip), %rsi
    mov  $PHYS_A, %rdi
    mov  $(code_a_end - code_a), %rcx
    rep movsb
    lea  code_b(%rip), %rsi
    mov  $PHYS_B, %rdi
    mov  $(code_b_end - code_b), %rcx
    rep movsb

    xor  %r12d, %r12d            /* sum of the return values */
    mov  $NROUNDS, %r13d
    mov  $CODE_VA, %rbx
3:  mov  $PML4_A, %rax
    mov  %rax, %cr3
    call *%rbx
    add  %eax, %r12d
    mov  $PML4_B, %rax
    mov  %rax, %cr3
    call *%rbx
    add  %eax, %r12d
    dec  %r13d
    jnz  3b

    mov  $0x20, %eax             /* failure, unless every check passes */
    cmp  $(NROUNDS * 3), %r12d
    jne  4f
    cmpl $0x11, (PHYS_A + 0x800)
    jne  4f
    cmpl $0x22, (PHYS_B + 0x800)
    jne  4f
    mov  $0x10, %eax
4:  mov  $0xf4, %dx
    out  %eax, (%dx)
hang:
    hlt
    jmp  hang

    /* A: mov $1,%eax (5 bytes); movl $0x11,DATA_VA; ret */
code_a:
    .byte 0xb8, 0x01, 0x00, 0x00, 0x00
    .byte 0xc7, 0x04, 0x25, 0x00, 0x08, 0x00, 0x40, 0x11, 0x00, 0x00, 0x00
    .byte 0xc3
code_a_end:
    /* B: xor %eax,%eax; inc %eax; inc %eax (2-byte first instruction);
       movl $0x22,DATA_VA; ret */
code_b:
    .byte 0x31, 0xc0, 0xff, 0xc0, 0xff, 0xc0
    .byte 0xc7, 0x04, 0x25, 0x00, 0x08, 0x00, 0x40, 0x22, 0x00, 0x00, 0x00
    .byte 0xc3
code_b_end:

    .align 16
gdt:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt

    .org 510
    .word 0xaa55
