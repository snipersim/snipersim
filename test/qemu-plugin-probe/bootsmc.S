/*
 * Code rewritten in place in a whole x86 machine, for the full-system trace
 * (testing/test_qemu.py, system-smc-rewrite).
 *
 * In real mode, straight from the boot sector: code A is copied to 0x8000
 * ("mov $1, %ax; ret", 4 bytes) and called; then code B is copied over it
 * ("xor %ax, %ax; inc %ax; inc %ax; ret", 5 bytes, a different first
 * instruction of a different length) and called; then A again.  The trace
 * must carry each call's own bytes at 0x8000.
 *
 * Exit (isa-debug-exit) 0x10 when the calls returned 1, 2 and 1; 0x20
 * otherwise.  QEMU reports (code << 1) | 1, so 33 is a pass.
 */
    .set SLOT, 0x8000

    .code16
    .section .text
    .globl _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    cld

    xor %bx, %bx                     /* running total of the checks */

    mov $code_a, %si
    mov $(code_a_end - code_a), %cx
    call install
    call SLOT
    cmp $1, %ax
    jne fail

    mov $code_b, %si
    mov $(code_b_end - code_b), %cx
    call install
    call SLOT
    cmp $2, %ax
    jne fail

    mov $code_a, %si
    mov $(code_a_end - code_a), %cx
    call install
    call SLOT
    cmp $1, %ax
    jne fail

    mov $0x10, %al
    jmp done
fail:
    mov $0x20, %al
done:
    mov $0xf4, %dx
    out %al, %dx
    hlt
    jmp done

/* Copy cx bytes from ds:si to SLOT.  A jump after it is all x86 asks of
   code that rewrites itself; QEMU notices the write to a translated page. */
install:
    mov $SLOT, %di
    rep movsb
    jmp 1f
1:  ret

code_a:
    mov $1, %ax
    ret
code_a_end:
code_b:
    xor %ax, %ax
    inc %ax
    inc %ax
    ret
code_b_end:

    .org 510
    .word 0xaa55
