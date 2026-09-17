Sniper's own UDB-format overlay, applied after the upstream RISC-V Unified
Database and the IME 0.9 contribution.  It carries only what those do not
define as instructions (vsetlambda).  Same layout as a UDB checkout, so
`gen_isa.py --udb <upstream> --udb <ime overlay> --udb tools/riscv/overlay`.
