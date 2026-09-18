# gemm3: vector and matrix space, validated four ways

`gemm3.c` computes one int32 matrix product, C = A x B^T (8x8x8), three
ways in one binary -- a scalar reference, RVV 1.0 (a vector dot product per
element) and IME 0.9 (2x2 tiles through `include/riscv_insn.h`) -- and
compares them: exit 100 when all agree, otherwise 4 plus a bit mask of
what differs (1 RVV, 2 IME, 4 AME).  `-DUSE_AME` adds a fourth path on
the AME draft (Ztt): 4x4 Int32 squares, strided loads of A and B rows,
`mmulbtacc.2d` per 4-wide K slice, `mmov.m.a` and a strided store per
output square, all through the same header.  QEMU does not know AME; the
frontend plugin executes it (`frontend/qemu-plugin/README.md`), so the
AME build dies with SIGILL when run without the plugin.  It is
freestanding and builds with a stock clang (`-march=rv64gcv
-ffreestanding`); `-DPLATFORM_HTIF -DNO_IME -Wl,-T,sail.ld` makes the
bare-metal variant the sail-riscv model runs (RAM at 0x80000000, HTIF
exit, vector unit enabled in mstatus).

Three harness cases in `testing/test_qemu.py` use it:

- `riscv-gemm-verify`: functional agreement under the IME QEMU.
- `riscv-gemm-simulate`: Sniper simulates the trace and
  `tools/riscv/expected_counts.py` checks every `riscv.<class>` counter and
  the recorded footprint bytes against the values the specification
  implies (tile geometry from the UDB's vtype definition, one dot product
  per element for the vector path): they must be exact.
- `riscv-ame-verify` / `riscv-ame-simulate`: the `-DUSE_AME` build under
  the plugin (exit 100, no `[AME] error` line), then the simulation with
  `expected_counts.py --ame`: the AME class counts (ownership,
  configuration, square loads and stores, `mmulbtacc.2d`, moves) and the
  extra footprint bytes must be exact as well.
- `riscv-gemm-sail`: the scalar-versus-RVV comparison on the sail-riscv
  RVV 1.0 model at VLEN=128, the golden functional reference (the model
  has no IME; the IME semantics are checked against the scalar reference
  under QEMU, whose helpers derive from the specification's Sail).
