#!/bin/sh
# The RISC-V / IME 0.9 chain, end to end, from the specification to a
# simulated workload:
#
#   1. tables     regenerate the decoder from the UDB (when SNIPER_UDB_ROOTS
#                 names the checkouts) and check the checked-in tables match
#   2. sniper     build the simulator with the RISC-V core model
#   3. qemu       QEMU riscv64 user mode with the IME patch and plugins,
#                 through deps/sniper-deps --riscv-ime (built once, cached)
#   4. compiler   a clang with a RISC-V target (SNIPER_RISCV_CC, or found on
#                 PATH); no rebuilt compiler: the matrix instructions come
#                 from the generated include/riscv_insn.h
#   5. workloads  test/qemu-plugin-probe/rv-ime.c and test/riscv-gemm/gemm3.c
#                 (scalar, RVV and IME, self-verifying), traced and simulated
#                 by the riscv-* cases of testing/test_qemu.py: exact counts,
#                 and the RVV path on the sail-riscv model (fetched with
#                 --riscv-ime) as the golden functional reference
#
# Environment: SNIPER_UDB_ROOTS="<riscv-unified-db>;<ime overlay>" (optional,
# step 1 is skipped without it), SNIPER_RISCV_CC (optional), JOBS.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
say() { printf '\n== %s\n' "$*"; }

say "1. decoder tables from the specification"
if [ -n "${SNIPER_UDB_ROOTS:-}" ]; then
   python3 testing/test_riscv_isa.py
else
   echo "SNIPER_UDB_ROOTS not set: using the checked-in tables (regeneration skipped)"
   SNIPER_UDB_ROOTS= python3 testing/test_riscv_isa.py
fi

say "2. Sniper with the RISC-V core model"
make -j"$JOBS" RISCV=ON QEMU=ON RISCV_IME=ON >/dev/null

say "3. QEMU riscv64 with the IME 0.9 patch"
python3 deps/sniper-deps --qemu --riscv-ime --jobs "$JOBS" fetch
QEMU_RISCV="$ROOT/qemu-riscv/build/qemu-riscv64"
[ -x "$QEMU_RISCV" ] || { echo "no $QEMU_RISCV"; exit 1; }
"$QEMU_RISCV" --version | head -1

say "4. a RISC-V compiler (stock clang)"
CC=${SNIPER_RISCV_CC:-}
if [ -z "$CC" ]; then
   for c in clang clang-19 clang-18 clang-17 clang-16 clang-15; do
      if command -v "$c" >/dev/null 2>&1 && "$c" --print-targets 2>/dev/null | grep -q riscv64; then CC=$(command -v "$c"); break; fi
   done
fi
[ -n "$CC" ] || { echo "no clang with a riscv64 target on PATH; set SNIPER_RISCV_CC (any distro clang >= 15 with lld)"; exit 1; }
"$CC" --version | head -1

say "5. the IME workload, traced and simulated"
SNIPER_QEMU_RISCV="$QEMU_RISCV" SNIPER_RISCV_CC="$CC" \
   python3 testing/test_qemu.py --case riscv-user-trace --case riscv-user-simulate \
                                --case riscv-ime-trace --case riscv-ime-simulate \
                                --case riscv-gemm-verify --case riscv-gemm-simulate --case riscv-gemm-sail
