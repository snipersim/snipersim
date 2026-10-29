/* The guest's time in full-system mode.  A boot sector (BIOS loads it at
 * 0x7c00, real mode, the BIOS's 18.2 Hz timer interrupt running) that
 * prints on COM1:
 *   rtc=YY-MM-DD hh:mm:ss    the CMOS clock as the guest finds it (BCD)
 *   loop=XXXXXXXX            rdtsc across a loop of 3 x LOOP instructions
 *   halt=XXXXXXXX            rdtsc across TICKS halts woken by the timer
 *   rtc2=..:..:..            the CMOS clock after them
 * and exits through isa-debug-exit (status 33).  record-trace runs
 * full-system QEMU on instructions (-icount shift=0,sleep=off), its clock
 * from Sniper's time zero (-rtc base=...,clock=vm): the loop then reads
 * about 3 x LOOP cycles (1 GHz, one instruction a nanosecond), the halts
 * about TICKS x 54.9 ms of guest time while the host waits for none of it,
 * and the clock starts at the time zero (testing/test_qemu.py system-time). */
    .set LOOP, 100000
    .set TICKS, 36

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

    mov  $msg_rtc, %si
    call puts
    call rtc_date
    mov  $' ', %al
    call putc
    call rtc_time
    call newline

    /* rdtsc across 3 x LOOP instructions */
    rdtsc
    mov  %eax, %edi
    mov  $LOOP, %ecx
1:  add  $1, %ebx
    sub  $1, %ecx
    jnz  1b
    rdtsc
    sub  %edi, %eax
    mov  $msg_loop, %si
    call puts
    call puthex32
    call newline

    /* rdtsc across TICKS halts, each woken by the BIOS's timer interrupt */
    rdtsc
    mov  %eax, %edi
    mov  $TICKS, %cx
    sti
2:  hlt
    loop 2b
    cli
    rdtsc
    sub  %edi, %eax
    mov  $msg_halt, %si
    call puts
    call puthex32
    call newline

    mov  $msg_rtc2, %si
    call puts
    call rtc_time
    call newline

    mov  $0xf4, %dx
    mov  $0x10, %eax
    outl %eax, %dx          /* QEMU exits (0x10 << 1) | 1 = 33 */
    hlt

/* COM1 output */
putc:
    push %dx
    mov  $0x3f8, %dx
    outb %al, %dx
    pop  %dx
    ret
puts:
    lodsb
    test %al, %al
    jz   3f
    call putc
    jmp  puts
3:  ret
newline:
    mov  $'\n', %al
    jmp  putc
/* al as two hex digits (a BCD byte prints as its two decimal digits) */
puthex8:
    push %ax
    shr  $4, %al
    call nibble
    pop  %ax
    and  $0x0f, %al
nibble:
    and  $0x0f, %al
    add  $'0', %al
    cmp  $'9', %al
    jbe  putc
    add  $7, %al
    jmp  putc
puthex32:
    mov  $4, %cx
    mov  %eax, %ebx
4:  rol  $8, %ebx
    mov  %bl, %al
    call puthex8
    loop 4b
    ret
/* the CMOS register al, in al */
cmos:
    outb %al, $0x70
    inb  $0x71, %al
    ret
rtc_date:
    mov  $0x09, %al
    call cmos
    call puthex8
    mov  $'-', %al
    call putc
    mov  $0x08, %al
    call cmos
    call puthex8
    mov  $'-', %al
    call putc
    mov  $0x07, %al
    call cmos
    jmp  puthex8
rtc_time:
    mov  $0x04, %al
    call cmos
    call puthex8
    mov  $':', %al
    call putc
    mov  $0x02, %al
    call cmos
    call puthex8
    mov  $':', %al
    call putc
    mov  $0x00, %al
    call cmos
    jmp  puthex8

msg_rtc:  .asciz "rtc="
msg_loop: .asciz "loop="
msg_halt: .asciz "halt="
msg_rtc2: .asciz "rtc2="

    .org 510
    .byte 0x55, 0xaa
