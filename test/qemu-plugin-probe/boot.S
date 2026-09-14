/* A boot sector with an exactly predictable instruction count.
 *
 * BIOS loads us at 0x7c00 in 16-bit real mode.  The outer loop runs
 * OUTER times; the inner loop body is a fixed number of instructions.
 * Everything we execute lives in [0x7c00, 0x7e00), so the plugin can
 * count precisely this and ignore firmware.
 */
    .set OUTER, 1000
    .set INNER, 1000

    .code16
    .section .text
    .globl _start
_start:
    cli                     /* 1 */
    xor  %ax, %ax           /* 2 */
    mov  %ax, %ds           /* 3 */
    mov  %ax, %es           /* 4 */
    mov  %ax, %ss           /* 5 */
    mov  $0x7c00, %sp       /* 6 */
    mov  $OUTER, %cx        /* 7  prologue = 7 instructions */

outer:
    mov  $INNER, %bx        /* 1 */
inner:
    add  $1, %ax            /* 2 */
    sub  $1, %bx            /* 3 */
    jnz  inner              /* 4  -> inner body is 3 insns per iteration */
    sub  $1, %cx            /* 5 */
    jnz  outer              /* 6 */

done:
    hlt                     /* halt; counted once, then loops */
    jmp  done

    .org 510
    .word 0xaa55
