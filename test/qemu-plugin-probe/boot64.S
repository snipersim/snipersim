/*
 * A guest that enables paging with a mapping we choose, so that VA -> PA
 * translation can be checked against a known answer rather than eyeballed.
 *
 * Boots at 0x7c00 in 16-bit real mode, builds 4-level page tables, enters long
 * mode, and then touches two addresses:
 *
 *   0x0000000000300000  identity mapped   -> PA 0x300000
 *   0x0000000040000000  NOT identity      -> PA 0x200000
 *
 * The second is the point: no identity-mapping shortcut can produce it, so a
 * plugin that reports PA 0x200000 for VA 0x40000000 is genuinely walking the
 * guest's page tables.
 *
 * Page tables are placed at physical 0x10000, clear of the boot sector.
 */
    .set PML4,  0x10000
    .set PDPT,  0x11000
    .set PD0,   0x12000          /* identity, first 1 GiB, 2 MiB pages */
    .set PD1,   0x13000          /* the non-identity window            */

    .set PRESENT_RW,      0x003  /* present | writable                 */
    .set PRESENT_RW_HUGE, 0x083  /* present | writable | 2 MiB page    */

    .set MARK_IDENT,  0x300000   /* identity-mapped probe address      */
    .set MARK_VIRT,   0x40000000 /* non-identity probe address         */
    .set MARK_PHYS,   0x200000   /* ... which must resolve to this     */

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

    /* Zero the four page-table pages: 4 * 4096 bytes from PML4. */
    mov  $PML4, %edi
    xor  %eax, %eax
    mov  $(4 * 4096 / 4), %ecx
    mov  %edi, %ebx
1:  movl %eax, (%ebx)
    add  $4, %ebx
    dec  %ecx
    jnz  1b

    /* Table entries are written through a register: a 16-bit absolute
     * displacement cannot hold these addresses and would silently truncate. */

    /* PML4[0] -> PDPT */
    mov  $PML4, %edi
    movl $(PDPT | PRESENT_RW), (%edi)

    /* PDPT[0] -> PD0  (covers VA 0 .. 1 GiB) */
    mov  $PDPT, %edi
    movl $(PD0 | PRESENT_RW), (%edi)
    /* PDPT[1] -> PD1  (covers VA 1 GiB .. 2 GiB, where MARK_VIRT lives) */
    movl $(PD1 | PRESENT_RW), 8(%edi)

    /* PD0: identity map the first 1 GiB with 2 MiB pages. */
    mov  $PD0, %ebx
    xor  %eax, %eax              /* physical address being mapped */
    mov  $512, %ecx
2:  mov  %eax, %edx
    or   $PRESENT_RW_HUGE, %edx
    movl %edx, (%ebx)
    movl $0, 4(%ebx)
    add  $0x200000, %eax
    add  $8, %ebx
    dec  %ecx
    jnz  2b

    /* PD1[0]: VA 0x40000000 -> PA MARK_PHYS.  The whole point. */
    mov  $PD1, %edi
    movl $(MARK_PHYS | PRESENT_RW_HUGE), (%edi)
    movl $0, 4(%edi)

    /* PAE on */
    mov  %cr4, %eax
    or   $(1 << 5), %eax
    mov  %eax, %cr4

    /* CR3 = PML4 */
    mov  $PML4, %eax
    mov  %eax, %cr3

    /* EFER.LME */
    mov  $0xC0000080, %ecx
    rdmsr
    or   $(1 << 8), %eax
    wrmsr

    /* Paging + protected mode */
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

    /* Touch the identity-mapped address. */
    mov  $MARK_IDENT, %rax
    movq $0x1111111111111111, %rbx
    mov  %rbx, (%rax)
    mov  (%rax), %rcx

    /* Touch the non-identity address.  A plugin reporting PA 0x200000 here is
     * walking the page tables; anything else is not. */
    mov  $MARK_VIRT, %rax
    movq $0x2222222222222222, %rbx
    mov  %rbx, (%rax)
    mov  (%rax), %rcx

    /* Prove the mapping really aliases: the value written through the virtual
     * window must be visible at its physical address, which is identity
     * mapped too. */
    mov  $MARK_PHYS, %rax
    mov  (%rax), %rdx

hang:
    hlt
    jmp  hang

    .align 16
gdt:
    .quad 0                                  /* null */
    .quad 0x00209A0000000000                 /* 64-bit code: L=1, present */
    .quad 0x0000920000000000                 /* data */
gdt_end:

gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt

    .org 510
    .word 0xaa55
