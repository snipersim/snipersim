# riscv-vlen-probe: the vector unit at its full width

`vlen_probe.c` runs every instruction family the agent's kernels use on a
whole register group (e32 m8, e8 m2) at the VLEN the CPU runs, against a
scalar reference, and exits 100 when all agree, otherwise 10 x family +
step (the table is at the top of the source).  The harness case
`riscv-vlen-probe` builds it and runs it at every power of two from 128
bits up to QEMU's cap, and passes when every length up to
`VLEN_KNOWN_GOOD` agrees; lengths above that are reported.  When QEMU's
VLEN cap was raised for the 32x32 IME tiles, this probe found that the
instructions QEMU expands through its generic vector code (`vadd.vi`,
family 5) return wrong data above 2 KB operands, a full m8 group at 4,096
bits, while the helper-based families are right; raising the cap to the
RVV 1.0 limit of 65,536 bits means fixing that first, and this is the
test that says when it is fixed.
