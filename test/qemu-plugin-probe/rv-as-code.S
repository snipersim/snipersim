    .option norvc
    .globl code_a, code_a_end, code_b, code_b_end
    .section .rodata
code_a:                  /* returns 1, stores 0x11 at VA 0x400007f0 */
    lui  t1, 0x40000
    li   t0, 0x11
    sw   t0, 0x7f0(t1)
    li   a0, 1
    ret
code_a_end:
    .option rvc
code_b:                  /* compressed first instruction: returns 2, stores 0x22 */
    c.li a0, 2
    .option norvc
    lui  t1, 0x40000
    li   t0, 0x22
    sw   t0, 0x7f0(t1)
    ret
code_b_end:
