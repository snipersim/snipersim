# Matrix acceleration in Sniper: the components, how they attach to the core, how they are programmed

This document describes every piece that carries a matrix instruction
or an accelerator task from a specification to a simulated cycle count:
the software that generates and runs it, the hardware models that time
it, how each unit connects to the core, how a program drives it, and the
commands that build and run the examples.  It ends with the measurements
of the two workloads on every unit.  The decoder generation is described in
`RISCV_ISA_GENERATION.md` (this directory).

## 1. The path at a glance

```
 specification                 software                                  hardware model (Sniper)
 -------------                 --------                                  -----------------------
 riscv-unified-db (ratified)   tools/riscv/gen_isa.py + isa_policy.yaml  decoder_lib/riscv_isa_gen.*  (1436 insns, 39 classes)
 IME 0.9 UDB (Zvvmm, Zvvfmm)   -> include/riscv_insn.h  (typed .insn     config/riscv.cfg [perf_model/core/riscv]  (class costs)
 AME draft -> gen_ame_udb.py      macros for any clang)                  core_model_boom_v1  (BOOM-class OoO core)
 Xsnacc overlay (task hints)                                             ame_tile.cc  (attached tile, DMA accelerator, couplings)
                               test/riscv-gemm/gemm3.c                   cache_cntlr.cc  (a port entering below L1)
                               test/llm-agent/agent.c
                                  | clang -march=rv64gcv (-I include)
                                  v
                               qemu-riscv64 (+IME patch) -plugin libsniper-qemu.so
                                  |  executes IME natively; AME and tasks in the plugin's models
                                  |  writes SIFT: instructions + memory footprints (MemorySpans)
                                  v
                               run-sniper -c riscv [-g perf_model/ame_tile/coupling=...]
```

Every stage after the specification learns the ISA from the same
generated tables: the plugin decides at translation time which
instructions to execute itself and which footprints to record, the trace
carries exact byte ranges, and Sniper's decoder, class costs, tile model
and expected-count checks all index the same class table.

## 2. Software components

### 2.1 Specifications and the generator

| component | what it is | where |
|---|---|---|
| Upstream UDB | The ratified RISC-V ISA as YAML (RVA profiles, RVV 1.0) | `riscv-unified-db` checkout (`SNIPER_UDB_ROOTS`) |
| IME 0.9 UDB | Zvvmm / Zvvfmm / Zvvmtls / Zvvmttls, the working group's own YAML | the ime-0.9-pipeline `public/udb` (second root) |
| AME overlay | The Attached Matrix Extension draft converted from AsciiDoc (`riscv-attached-matrix-extension`) into UDB YAML, 138 instructions, every encoding checked against the repository's mask/match table | `tools/riscv/gen_ame_udb.py` -> `tools/riscv/overlay/.../inst/Ztt` (`SNIPER_AME_ROOT` regenerates it) |
| Xsnacc overlay | Sniper's two task hints for the DMA-attached accelerator, `acc.task xs1` and `acc.wait` (CUSTOM-3 opcode) | `tools/riscv/overlay/.../inst/Xsnacc`, hand-written |
| Policy | Which extensions to take, how to class instructions (regexes on extension, name, assembly), placeholder latency and issue interval per class, class flags (load, store, footprint, matrix, ...) | `tools/riscv/isa_policy.yaml` |
| Generator | Reads the roots in order (later roots override), applies the policy, emits the decoder tables, the class table, the `.insn` header and the config blocks | `tools/riscv/gen_isa.py` |
| Drift test | Regenerates into a temporary directory and fails if the checked-in tables differ; decodes 20 known words | `testing/test_riscv_isa.py` |

The generated outputs are checked in: `decoder_lib/riscv_isa_gen.{h,cc}`
(instruction table, dispatch buckets, class table), `include/riscv_insn.h`
(one macro per instruction of the extensions the policy's `header:`
section names), and the `[perf_model/core/riscv]` block between the
`BEGIN/END generated` markers of `config/riscv.cfg` and
`config/riscv-mediumboom.cfg`.  Anything hand-written in those files must
sit outside the markers.

### 2.2 The header: programming a unit from C with any compiler

`include/riscv_insn.h` turns every IME, AME and Xsnacc instruction into a
typed `.insn` macro, so a stock clang (any distribution's clang 15 or
newer with `-march=rv64gcv`) assembles them without knowing the
extensions.  Register operands are macro parameters that become register
names; X-register sources are C expressions passed through `"r"`
constraints; fixed selector fields of AME's R2/R1 forms are encoded as
the register name carrying that value.  Examples, one per unit:

```c
/* IME 0.9: a 2x2 fp32 tile op.  v0 <- two rows of W, v8 <- two rows of X,
   v16 += v0 x v8^T.  lambda 2 rows, ld in elements. */
RISCV_VSETLAMBDA(old, 2L);
RISCV_VMTL_V(0,  W + o * nin + k, (long)nin, 2, 1);
RISCV_VMTL_V(8,  X + t * ldx + k, (long)ldx, 2, 1);
RISCV_VFMMACC_VV(16, 0, 8, 1);
RISCV_VMTS_V(16, Y + t * ldy + o, (long)ldy, 2, 1);

/* AME draft: a 4x4 fp32 square op.  Ownership, datatypes, strided
   square loads, C += A x B^T, move to an M register, strided store. */
RISCV_AME_ACQUIRE(status, 0L);
RISCV_MSETTYP(0, 0x20300120L); RISCV_MSETTYP(1, 0x20300120L); RISCV_MSETTYP(2, 0x20300120L);
RISCV_ASETTYP(0, 0x20300120L);                      /* zeroes accumulator 0 */
RISCV_MLS_ST(0, X + t * ldx + k, (long)ldx * 4);    /* stride in bytes */
RISCV_MLS_ST(1, W + o * nin + k, (long)nin * 4);
RISCV_MMULBTACC_2D(0, 0, 1);
RISCV_MMOV_M_A(2, 0);
RISCV_MSS_ST(2, Y + t * ldy + o, (long)ldy * 4);
RISCV_AME_RELEASE();

/* DMA-attached accelerator: a task descriptor, issue, wait. */
struct acc_desc { const float *x, *w, *bias; float *y; unsigned t, nin, nout, pad; } d = { X, W, bias, Y, T, nin, nout, 0 };
RISCV_ACC_TASK(&d);
/* ... independent work may go here ... */
RISCV_ACC_WAIT();
```

Register-numbered operands (the tile or accumulator index) must be
literals, since they become part of the instruction word; a kernel that
indexes accumulators in a loop dispatches through a `switch`
(`SEL16`/`SELV` in `test/llm-agent/agent.c`).

### 2.3 QEMU and the frontend plugin

| piece | role |
|---|---|
| `qemu-riscv/` | QEMU 11.1.1 built by `deps/sniper-deps --riscv-ime` with `tools/riscv/qemu-11.1.1-ime.patch`: IME decode, helpers and the `x-zvvmm`, `x-zvvfmm`, `x-zvvmtls`, `x-zvvmttls` CPU properties. Run with `-cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on`. |
| `frontend/qemu-plugin/sniper_qemu.cc` | The tracer: classifies every translated instruction with the generated tables, records conditional branches, and for `footprint` classes records every byte range the instruction touches, coalesced into contiguous runs (`MemorySpans`, up to 16 per instruction). |
| `frontend/qemu-plugin/ame_backend.{h,cc}` | The AME functional model (M registers, accumulators, datatypes Int32 and FP32, ownership, 4x4 squares). Registered as a second callback with register/PC write rights; the instruction never reaches QEMU's illegal-instruction path. |
| `frontend/qemu-plugin/acc_backend.{h,cc}` | The DMA-attached accelerator's functional model: `acc.task` reads the descriptor and inputs and computes the product; `acc.wait` writes the output. Each block is one footprint. |

QEMU has no AME and no accelerator, and no patch was added for either:
the plugin executes them through the v7 plugin API (register read/write,
guest memory read/write, set-PC).  The cost of that route is one
translation-block exit per instruction.

The AME and Xsnacc instructions run on models inside the plugin.  With
the plugin-API additions the patch makes to QEMU (frontend/qemu-plugin/README.md,
"Emulating an extension") they are claimed at translation, so the block
runs through them like any other instruction; the models raise the
draft's exceptions (illegal instruction, access faults) as real ones; the
unit's CSRs answer `csrr`; and its registers (CSRs, datatype registers,
M and accumulator squares) are in the vcpu's gdb register list.  The
harness case `riscv-plugin-api` checks each.

### 2.4 Workloads

| workload | paths | self-check | counts |
|---|---|---|---|
| `test/riscv-gemm/gemm3.c` | scalar, RVV, IME 0.9 (2x2 int32 tiles), AME (4x4 Int32 squares, `-DUSE_AME`); `-DM/-DN/-DK` size, `-DAME_REPEAT` for timing runs; `-DPLATFORM_HTIF` bare-metal variant for sail-riscv | exit 100 only when every path agrees with the scalar reference | `tools/riscv/expected_counts.py [--ame]` derives every class count and footprint byte from the geometry |
| `test/llm-agent/agent.c` | a 159,936-parameter tool-calling GPT: scalar, RVV, IME, AME, DMA tasks (`-DUSE_RVV/-DUSE_IME/-DUSE_AME/-DUSE_DMA`); prefill runs the queued prompt as one batch (the class B GEMM shape), decode is token-serial | every generated line against the numpy model's greedy output, every answer against the tool; exit 100 | the harness derives tile, square and task counts from the model shape and the printed prefill batch sizes |

### 2.5 The harness

`testing/test_qemu.py --case <name>` (needs `SNIPER_QEMU_RISCV`,
`SNIPER_RISCV_CC`; `SNIPER_SAIL_RISCV` optional):

| case | what it proves |
|---|---|
| `riscv-gemm-verify`, `riscv-gemm-simulate`, `riscv-gemm-sail` | scalar/RVV/IME agree under QEMU and on the sail-riscv RVV 1.0 model; Sniper's class counts and footprint bytes equal the specification's |
| `riscv-ame-verify`, `riscv-ame-simulate` | the AME path agrees (plugin execution); AME class counts exact |
| `riscv-ame-couplings` | a 32x32x32 AME product, repeated, under `ica`, `oca_l1`, `oca_l2`, `oca_llc`, `nca`: tile ops, rows, squares and prefetches exact, cycles respond |
| `riscv-llm-agent` | the agent's five kernels reproduce the numpy model on eight prompts |
| `riscv-llm-simulate` | RVV, IME and AME (in-core costs) simulated: cycles per token, matrix counts exact |
| `riscv-llm-couplings` | AME on the tile and the DMA accelerator under `oca_l2` and `nca`: counts exact, cycles per token |

`make riscv-ime-check` (`tools/riscv/ime-check.sh`) runs the chain end to
end from the dependencies.

## 3. Hardware components

### 3.1 The core

`config/riscv.cfg` + `config/riscv-mediumboom.cfg`: a BOOM-class
out-of-order core (`core_model_boom_v1`), ROB timer, L1I/L1D, a private
L2 as the last level in this configuration, DRAM.  Every generated
instruction class has a latency and an issue interval in the
`[perf_model/core/riscv]` block, read by the core model; classes with the
`matrix` flag occupy the matrix port for their issue interval, `busy`
classes (dividers) for their latency.

### 3.2 IME: the in-core matrix unit

IME is an in-core accelerator in ATX's terms: tiles live in the vector
register file, tile loads and stores are ordinary memory instructions of
the core, `vmmacc.vv`/`vfmmacc.vv` are functional-unit operations.  In
the model a tile memory instruction is given `perf_model/core/riscv`'s
`memory_uops` load or store micro-ops; each recorded row takes one, the
core squashes rows that fall on a cache line already touched by the same
instruction, and spare micro-ops are squashed.  Multiply-accumulates are
costed by class (`ime_mma_int`, `ime_mma_fp`).

### 3.3 AME: the attached tile (`ame_tile.cc`)

The tile is where AME instructions execute when the unit is not part of
the core's pipeline.  The core's ROB still holds every AME micro-op, so
the M and accumulator registers give real dependencies and the window
fills when the tile is slow; the op's execution time, and its rows'
memory accesses, come from the tile:

- an in-order queue fed at issue (`dispatch_latency` cycles after the
  core issues), `mma_issue_cycles` per square, `bytes_per_cycle` on the
  port, `mma_latency` to a result, `result_latency` for an X-register
  result to return;
- its own memory port, entering the hierarchy at the level the coupling
  names (section 4);
- for the near-core coupling, a transfer engine that fetches a square's
  rows when the instruction is dispatched, ahead of the tile executing
  it (`prefetch_hits`).

Statistics under `ame_tile.<core>`: `ops`, `rows`, `bytes`, `mma`,
`busy`, `queue_wait`, `idle`, `mem_latency`, `prefetch_hits`.  Ownership
across cores is not arbitrated yet: one tile per core.

### 3.4 The DMA-attached accelerator (`ame_tile.cc`, Xsnacc)

The same datapath and port, driven by tasks instead of instructions.
`acc.task`'s footprints (descriptor, X, W, bias) stream in through the
port behind the previous task; the compute runs at `macs_per_cycle` once
the inputs are in (work derived from the operand sizes, T x nin x nout);
`acc.wait`'s footprint (Y) streams out.  The core retires `acc.task`
after the dispatch cost and continues; `acc.wait` costs whatever remains
of that timeline plus `result_latency`.  Statistics: `tasks`,
`task_bytes_in`, `task_bytes_out`, `task_macs`, `task_dma`,
`task_compute`, `task_wait`.

### 3.5 The cache path below L1

A port that enters below L1 still goes through the L1 controller, which
owns the locks, but carries a memory origin (`Core::ACCEL_L2`,
`Core::ACCEL_LLC`) that makes the levels above the port pass the request
down without looking it up or filling themselves.  A tile write
invalidates the core's copy of the line, writing a dirty copy back
first, as coherence would.  `MemoryManager::coreInitiateMemoryAccess`
translates a below-L1 component into the origin; `Core::accessMemoryAt`
is the entry the tile uses.

## 4. How each unit connects to the core

| | IME | AME, `ica` | AME tile, `oca_l1` / `oca_l2` / `oca_llc` | AME tile, `nca` | DMA accelerator |
|---|---|---|---|---|---|
| Invocation | instructions in the pipeline | instructions in the pipeline | instructions, queued to the tile at issue | same | `acc.task` with a descriptor in memory |
| Operand state | vector registers | modelled as functional-unit ops | M registers and accumulators in the tile | same | memory blocks |
| Data path | the core's L1D through its load/store unit | the core's L1D | the tile's port at L1D / L2 / LLC | LLC plus a transfer engine that prefetches rows at dispatch | the tile's port at the coupling's level, streamed by a DMA engine |
| What the ROB sees | load/store/matrix micro-ops with core latencies | class latencies | micro-ops whose latency is the tile's completion; dependencies through M/acc registers | same | `acc.task` retires at once; `acc.wait` waits for the task |
| Synchronisation | none needed | none | in order on the tile, dependencies in the ROB | same | `acc.wait` |
| Config | `perf_model/core/riscv/ime_*` | `perf_model/core/riscv/ame_*` | `perf_model/ame_tile/coupling` and its parameters | `coupling = nca` | `perf_model/ame_tile/macs_per_cycle` and the port parameters |

With the two-level cache configuration of `config/riscv.cfg`, `oca_llc`
is `oca_l2`; a configuration with an L3 separates them.

## 5. How each unit is programmed

- **IME** (`test/llm-agent/agent.c -DUSE_IME`, `gemm3.c gemm_ime`): set
  `vtype` to e32,m1 and `lambda` to 2; tiles are 2x2 fp32 (VLEN=128); a
  weight tile is loaded once per group of up to 16 activation tiles held
  in v16..v31; column 0 of a tile is two outputs for a batch-1 matvec, the
  whole tile is two tokens' outputs in the prefill GEMM.
- **AME** (`-DUSE_AME`, `gemm3.c gemm_ame`): acquire the unit, program
  the datatype (Int32 `0x40000020` or FP32 `0x20300120`) into the M
  registers and each accumulator (`asettyp` also zeroes it), strided
  square loads of four rows, `mmulbtacc.2d` per 4-wide K slice, move the
  accumulator to an M register and store it.  The 16 accumulators hold
  16 token squares, so a weight square is loaded once per 64 tokens.
- **DMA accelerator** (`-DUSE_DMA`): fill the descriptor (contiguous X,
  W, bias, Y blocks and T, nin, nout), `acc.task`, do independent work,
  `acc.wait` before reading Y.  The agent issues one task per weight
  product: 13 per decode token, 13 per prefill batch.
- **Adding an instruction**: write its UDB YAML in an overlay, give it a
  class in the policy, regenerate (section 6); the plugin executes it if
  a model in `ame_backend.cc`/`acc_backend.cc` claims its class, the tile
  times it if the class name starts with `ame_` or `acc_`.

## 6. Build and run

Dependencies and Sniper (from the repository root):

```
python3 deps/sniper-deps --qemu --riscv-ime fetch      # QEMU 11.1.1 + IME patch -> qemu-riscv/, sail-riscv/
make -j8 RISCV=ON QEMU=ON RISCV_IME=ON                  # Sniper, the RISC-V decoder, the frontend plugin
export SNIPER_QEMU_RISCV=$PWD/qemu-riscv/build/qemu-riscv64
export SNIPER_RISCV_CC=clang-15                          # any clang >= 15 with a riscv64 target and lld
```

Regenerating the decoder after a specification or policy change:

```
export SNIPER_UDB_ROOTS="<riscv-unified-db>;<ime-0.9-pipeline>/public/udb"
export SNIPER_AME_ROOT=<riscv-attached-matrix-extension>        # optional: regenerate the AME overlay
python3 tools/riscv/gen_isa.py --udb <riscv-unified-db> --udb <ime udb> --udb tools/riscv/overlay \
        --policy tools/riscv/isa_policy.yaml --out decoder_lib \
        --config config/riscv.cfg --config config/riscv-mediumboom.cfg --header include/riscv_insn.h
python3 testing/test_riscv_isa.py                                # drift and known-word checks
```

Building and running the examples by hand (`CC` is the clang above,
`Q` the QEMU, `P` `build/default/lib/libsniper-qemu.so`,
`CPU=max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on`):

```
# gemm3 on all four paths, then simulated with the AME instructions on the tile at L2
$CC --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding -nostdlib -static \
    -fuse-ld=lld -Wl,-e,_start -I include -DUSE_AME -DM=32 -DN=32 -DK=32 -o gemm3 test/riscv-gemm/gemm3.c
$Q -cpu $CPU -plugin $P,output=g3 -- ./gemm3 ; echo $?          # 100: scalar, RVV, IME, AME agree
./run-sniper -n 1 -c riscv -d out-g3 -g perf_model/ame_tile/coupling=oca_l2 --traces=g3.app0.th0.sift
python3 tools/riscv/expected_counts.py --ame --m 32 --n 32 --k 32 out-g3/simulation/sim.stats
grep '^ame_tile' out-g3/simulation/sim.stats

# the LLM agent on each unit (IME needs x-zvvfmm; AME and DMA need the plugin)
for v in "" -DUSE_RVV -DUSE_IME -DUSE_AME -DUSE_DMA; do
  $CC --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding -nostdlib -static \
      -fuse-ld=lld -Wl,-e,_start -I test/llm-agent -I include $v -o agent$v test/llm-agent/agent.c
done
$Q -cpu $CPU -plugin $P,output=llm -- ./agent-DUSE_AME ; echo $?   # 100: every line as numpy, every answer right
./run-sniper -n 1 -c riscv -d out-llm --traces=llm.app0.th0.sift                                   # in-core costs (ica)
./run-sniper -n 1 -c riscv -d out-llm-nca -g perf_model/ame_tile/coupling=nca --traces=llm.app0.th0.sift
```

The harness cases of section 2.5 do all of the above with the exact
count checks; `make riscv-ime-check` runs them in sequence.

## 7. Results: both workloads on every unit

All runs: Sniper with `config/riscv.cfg` (a single core with BOOM v3
MegaBoomConfig's parameters at 2 GHz: 4-wide, 128-entry ROB, 32-entry load
queue, an L1-D with 8 MSHRs, and a 1 MiB L2 with base.cfg's 32 MSHRs that
this one core has to itself; memory at 25.6 GB/s), every count checked against what the
workload's shape implies (the harness cases of section 2.5).

**Re-measured for 9.0 (2026-10-26).**  The tables in sections 7 to 9 are
from the configuration as released.  Before 9.0 `riscv.cfg` held a
Cortex-A72's parameters under a RISC-V name, among them an L1-D with no
limit on outstanding misses; BOOM's eight MSHRs are what moved the
results, and only the rows that issue many lines at once moved much.  The
small model's RVV row went from 82,322 to 109,682 cycles per token, and
81,460 with the L1-D unlimited again, so the MSHRs alone account for it;
the 4,096-bit vector row went from 43,523 to 104,006, and 43,161 with the
L1-D unlimited.  The scalar, matrix and accelerator rows moved by under 2
percent.  Comparisons in the text with figures from before the vector
kernels or the batched prefill are kept as measured, on the earlier
configuration, and say so.  The rows recorded live for each run vary
from run to run by up to 3 percent (the 32x32 IME row has measured
33,327 to 34,235), since the recording itself is not deterministic.  The small model fits in the L2, so its rows see about 33 DRAM
reads per token and no writes; the memory side only matters for the
100 MiB model of section 9.

**The LLM agent**, 379 tokens (276 prefill tokens in 16 batches of 8 to
26, 103 decoded), every kernel reproducing the numpy model's tokens:

| kernel | unit and coupling | cycles / token | instructions / token | vs RVV |
|---|---|---|---|---|
| scalar (F extension) | core | 693,165 | 1,162,677 | 0.16x |
| RVV 1.0, e32 m4 | core | 109,682 | 133,436 | 1.00x |
| int4 weights, dequantised on RVV (`-DUSE_W4`) | core | 67,595 | 107,784 | 1.62x |
| IME 0.9, fp32 2x2 tiles | in-core | 206,291 | 187,128 | 0.53x |
| AME draft, fp32 4x4 squares | in-core (`ica`) | 69,697 | 68,616 | 1.57x |
| AME draft | attached tile, port at L2 (`oca_l2`) | 86,769 | 68,616 | 1.26x |
| AME draft | attached tile, near-core with transfer engine (`nca`) | 85,471 | 68,616 | 1.28x |
| DMA-attached accelerator, one task per weight product | tile datapath, port at L2 | 25,551 | 19,412 | 4.29x |
| DMA-attached accelerator | near-core | 25,551 | 19,412 | 4.29x |

Every kernel outside the weight products now runs on the vector unit
too: layer normalisation, GELU and the attention over the KV cache
(softmax and the weighted sum of the value rows), as strip loops that
run at any VLEN; the harness derives every vector-class instruction
count of the RVV build from the model's shape and requires it exact.
That work had been the floor under every row: with it vectorised the
RVV row went from 138,687 to 82,322 cycles per token, and the DMA
accelerator, whose time was almost all that scalar work between tasks,
from 86,646 to 26,240 (its 13 tasks per token cost 9,275 cycles of
compute; both on the configuration before 9.0).  The int4 row
dequantises inside the dot product: 8x fewer weight bytes, and 38
percent fewer cycles than RVV on a model that fits in the L2, since its
loads are an eighth of the lines and the eight MSHRs hold it back
less (17 percent fewer on the configuration before 9.0).  The tile kernels dispatch each step over a group's token blocks
through one switch that falls through the blocks, one indirect jump per
step instead of one per block, which took the 32x32 rows of section 8
to half.

The same kernels with the prefill token-serial, the shape measured
before the class B kernel existed (on the configuration before 9.0): RVV 141,698, IME 472,610, AME
185,668 cycles per token.  Batching the prefill took IME from 3.3x to
2.1x slower than RVV and AME from 1.3x slower to 1.07x faster, with the
same instruction-level counts formulas confirming that every prefill
GEMM filled its tiles (a batch shorter than a tile is padded; the
figures are the padded kernels').

**gemm3 on the attached tile**, the 32x32x32 AME product repeated 16
times (`riscv-ame-couplings`): 97,360 tile ops, 69,632 rows, 8,192
squares under every coupling; cycles `ica` 387,000, `oca_l1` 413,000,
`oca_l2` 413,400, `oca_llc` 413,400 (the LLC is the L2 here), `nca`
413,400.

What the numbers say:

- **The matrix units need the matrix shape.**  Token-serial prefill
  gave both units one useful column per tile; the batched prefill fills
  them.  AME's 4x4 squares and 16 accumulators let a weight square be
  loaded once per prompt, which is why it beats the vector unit (1.57x)
  even though decode (a quarter of the tokens) is still one column per
  square.  IME's 2x2 tiles at VLEN=128 carry too little work per
  instruction to catch up: token-serial, 452,697 instructions per token
  against RVV's 211,559; batched, 187,128 against 133,436.
- **The attached tile costs about 25 percent over the in-core cost on
  this workload** (11 percent before the vector kernels shrank the rest),
  from the dispatch latency and the in-order queue: the tile executes
  45,876 ops per token in program order, and the ROB fills behind it.  Moving the port from L1 to L2 changed little, because the
  tile pipelines its rows and the out-of-order window covers their
  latency; the near-core transfer engine prefetched every row and gained
  1.5 percent for the same reason.  On the pure GEMM the attached
  couplings are within 0.1 percent of each other, 7 percent above the
  in-core unit.  Where the port sits will matter for a
  workload that misses in L2, and for several units sharing it.
- **The DMA-attached accelerator is the fastest configuration**, 4.3x
  over RVV and 2.7x over AME in the core.  Per token: 13 tasks, 148,360
  multiply-accumulates, 9,275 cycles of compute at 16 per cycle, 3,190
  cycles of streaming, 196 KB streamed in (weights once per batch in
  prefill, once per token in decode), and about 13,000 cycles stalled at
  `acc.wait` (the wait holds the instruction window until the task's
  output is written; this breakdown is from the configuration before
  9.0, when the row was 26,240).  The rest, about 13,000 cycles per token, is the
  core's own work between tasks, now on the vector unit: attention over
  the KV cache, layer normalisation, GELU, the embedding and cache
  writes.  Before those kernels were vectorised that work was 75,000
  cycles and the whole row 86,646; the wait stall is now the largest
  single term, and issuing the next independent task before waiting on
  the previous one (the kernel waits immediately today) is the next step
  on this path.
- **Near-core and L2 are identical for tasks**, because a task's blocks
  are streamed as a whole when the task starts; the transfer engine's
  prefetch-at-dispatch applies to instructions, not to tasks.

## 8. How big can the units be, and what a 32x32 unit buys

Every unit's size is a parameter, and every part of the chain now carries
32x32.  The ceilings, from the specifications and the tools:

| unit | what sets the size | ceiling | here |
|---|---|---|---|
| RVV 1.0 vector unit | VLEN, a power of two of bits per register; LMUL groups up to 8 registers | 65,536 bits (the specification); QEMU 11.1.1 stops at 1,024, the IME patch takes it to 65,536 (`test/riscv-vlen-probe` checks every VLEN) | 128 (default), 4,096 and up to 65,536: 4 to 2,048 fp32 elements per register, 16,384 per m8 group |
| IME 0.9 tiles | rows = (VLEN/SEW)/lambda, columns = lambda x LMUL, so a square tile has VLEN/SEW = lambda^2 x LMUL, and lambda must be one of the two or three values the freeze allows for that VLEN/SEW | square fp32 tiles: 2x2 (VLEN 128, m1), 4x4 (256, m2), 8x8 (512, m4), 16x16 (1,024, m8: stock QEMU's limit), 32x32 (4,096, m8, lambda 4), 64x64 (16,384), 128x128 (65,536, the RVV maximum) | 2x2 and 32x32 measured; the patched QEMU runs every size to 128x128 (helper buffers sized for a whole group, generic-vector and page limits fixed) |
| AME draft squares | AME_NELEM, any power of 4; N = sqrt(AME_NELEM); AME_MAX_SQUARE_SIZE is an implementation parameter with no ceiling in the draft | unbounded by the draft; the plugin model takes `ame_n` up to 32 (192 KB of unit state at 32x32 fp32: 32 M registers of 4 KB and 16 accumulators), the tile model reads N from the rows it loads | 4x4 and 32x32 |
| DMA-attached accelerator | none: blocks of any size; the compute rate `macs_per_cycle` is the unit's size | -- | 16 and 1,024 multiply-accumulates per cycle |
| The trace and the core model | one span, one memory entry and one memory micro-op per row: `Sift::MAX_MEMORY_SPANS`, `DynamicInstruction::MAX_MEMORY`, the policy's `memory_uops` | 32 rows per instruction timed; beyond that the plugin coalesces by cache line, times the first 32 lines and counts the rest (`riscv.spans_dropped_lines`), so a 16,384-line gather at 65,536 bits is recorded whole; `vector_bytes` goes to 8,192 and the operand size is 32-bit | -- |

The software follows the geometry at compile time: `-DAME_N=32` for the
agent and gemm3 (`ame_n=32` to the plugin), `-DIME_VLEN=4096 -DIME_LMUL=8
-DIME_LAMBDA=4` for IME (`-cpu ...,vlen=4096` to QEMU), the RVV kernel
adapts through `vsetvli`.  Batches shorter than a tile are padded; the
harness cases `riscv-size32-verify` (gemm3 at 64x64x64) and
`riscv-llm-size32` derive the counts from the geometry.  In the
simulation the wide units are charged accordingly: a 32x32 square
product is 32,768 multiply-accumulates, issued every 32 cycles on a
32x32 array (the tile derives that from the rows it loads; the in-core
class costs take `ame_mma_issue_cycles=32` and `ime_mma_fp_issue_cycles=32`),
and vector operands are 512 bytes.  The vector class costs are per
instruction by default, a datapath as wide as the register group, so the
4,096-bit vector row is an upper bound; `perf_model/core/vector/datapath_bytes`
gives the unit a width, and a vector instruction then holds its port for
the cycles its group (vector_bytes times the assumed `lmul`) takes through
it -- with a 16-byte datapath the same 4,096-bit run is 7x slower
(`riscv-vector-datapath`).

**The LLM agent at both geometries**, cycles per token, 379 tokens:

| kernel | at the baseline geometry | at 32x32 (RVV: 4,096 bits) | change |
|---|---|---|---|
| RVV 1.0 | 109,682 (VLEN 128) | 104,006 | 1.05x |
| IME 0.9 in core | 206,291 (2x2) | 34,235 | 6.0x |
| AME in core | 69,697 (4x4) | 36,075 | 1.93x |
| AME on the tile, near-core | 85,471 (4x4) | 39,565 | 2.16x |
| DMA accelerator | 25,551 (16 MACs/cycle) | 16,423 (1,024 MACs/cycle) | 1.56x |

The 4,096-bit vector unit gains 5 percent over 128 bits: each of its
loads covers up to 32 cache lines, and on an L1-D miss those queue for
the eight MSHRs.  With the L1-D unlimited, as the configuration had it
before 9.0, the same trace runs at 43,161 cycles per token, 2.5x faster
than at 128 bits (81,460 there).  A wide vector unit needs either MSHRs
in proportion to its footprint or a load path that does not go through
the L1-D's; the AME tile's near-core transfer engine is the second.

Instructions per token: AME 68,616 to 48,063, IME 187,128 to 44,667,
RVV 133,436 to 62,866.  (Two changes since the first 32x32 measurement
of 91,000 to 102,000 per row: the fall-through block dispatch, with 16
token blocks per group the per-block switch had cost as much as the
matrix work, and the vector kernels for everything outside the weight
products.)  Every count is exact at both geometries; the
AME 32 run executes 17,374 squares, which is what 6+2+8+2 output blocks
of 32 rows over 2 K-slices per layer, 3 layers, the head and the 16
prefill batches imply, and the tile receives every byte of every square
(`riscv-llm-size32` checks the rows and bytes: an earlier decoder cap of
16 memory micro-ops per instruction had delivered half of each 32-row
square, which is why the 32x32 rows moved by 1 to 3 percent when it was
lifted).

What the numbers say:

- **Bigger tiles help, and the dispatch must keep up.**  The two
  matrix units land within 10 percent of each other at 32x32, 34,000 to
  36,000 cycles per token, against 104,000 for the vector unit at 4,096
  bits (43,000 with an unlimited L1-D); the DMA accelerator is 25,551 at
  16 multiply-accumulates per cycle and 16,423 at 1,024, because its weight products cost no
  instructions at all and the wait on each task is what remains.  With the activations
  and attention on the vector unit, the remaining floor under every row
  is the embedding, the KV-cache writes and the scalar control between
  kernels.
- **At batch 1 the wide unit is almost all padding.**  A decode matvec
  fills one column of a 32x32 square: 32,768 multiply-accumulates for
  1,024 useful ones, 3 percent utilisation.  The prefill batches of 8 to
  26 tokens fill 25 to 81 percent of a token block.  The AME 32 run does
  569 million multiply-accumulates on the array for the 56 million the
  model needs; the 4x4 run did 4.5x fewer wasted ones.  The gain came
  from fewer, larger instructions, not from more useful work per cycle.
- **This model is too narrow to feed a 32x32 unit.**  With d = 64 a
  square product covers 2 K-slices; the weight matrices are 2 to 8 output
  blocks wide.  A 32x32 unit wants d of 512 or more and batches of 32
  tokens: prefill-shaped class B work, or decode batched by speculative
  verification.  The measurement to make next is a wider
  agent (d = 256 or 512) at both geometries, which the trainer can
  produce and every count formula already covers.
- **IME gains the most** because its 2x2 tiles were the least efficient
  encoding of the work; at 32x32 IME and AME are equivalent in-core
  units, and the tile's attachment cost (dispatch and in-order queue) is
  10 percent at 32x32 against 23 percent at 4x4, since there are fewer,
  longer operations to queue.
- **Ownership and context state grow with the square.**  A 32x32 AME
  unit holds 192 KB of architectural state; the draft's whole-register
  image transfers (`mls.1r`/`mss.1r`) are what a context switch moves,
  and the tile model does not charge for them yet.

## 9. The 100 MiB agent: where the bytes are

Sections 7 and 8 measured a model whose weights fit in the L2, which is
the one case where a compressed weight format cannot show as time.
`test/llm-agent-wide` is the same agent at d = 512, 8 layers, context
128: 25.3 million parameters, 101 MB of fp32, so every decode token
streams the whole model through a memory system that holds none of it.
The model is untrained (the trainer's export-anyway mode: the references
are the untrained numpy model's own outputs, and the generation lengths
are pinned to the numpy value, so the self-check still holds), and its
weights travel as a binary blob embedded at link time.  A second weight
format joins fp32: int4 with a per-row scale (`quantize_w4.py`, 12.7
MB, dequantised on the vector unit inside the dot product).  One prompt,
8 tokens per generate, `tools/llm/wide-sweep.sh` at 25.6 GB/s of memory
bandwidth (an edge figure, and `config/riscv.cfg`'s default):

| kernel | weight format | unit | cycles / token | instructions / token | DRAM reads + writes / token | bytes / token |
|---|---|---|---|---|---|---|
| fp32, 32x32 squares | fp32, 101 MB | AME in core | 9,554,188 | 964,436 | 580,555 + 6,574 | 37.6 MB |
| fp32, 32x32 squares | fp32 | AME tile, near-core | 4,734,933 | 964,436 | 580,600 + 6,599 | 37.6 MB |
| int4, dequantised in the dot product | int4, 12.7 MB | RVV, 128 bits | 12,397,708 | 9,735,916 | 83,910 + 3,691 | 5.6 MB |
| int4 | int4 | RVV, 4,096 bits over a 64-byte datapath | 11,003,144 | 1,741,848 | 83,910 + 3,691 | 5.6 MB |
| fp32 direct, token-serial prefill | fp32 | RVV, 128 bits | 44,842,424 | 15,127,571 | 1,528,660 + 5,098 | 98.2 MB |
| fp32 direct | fp32 | RVV, 4,096 bits over a 64-byte datapath | 63,347,412 | 1,258,098 | 1,529,268 + 5,098 | 98.2 MB |

| row | DRAM floor (cycles / token) | share of the bandwidth used | DRAM reads in flight | time stalled on DRAM |
|---|---|---|---|---|
| fp32 squares, AME in core | 2,902,773 | 30% | 13.2 | 91% |
| fp32 squares, near-core tile | 2,903,000 | 61% | 31.9 | 17% |
| int4, RVV 128 bits | 419,552 | 3% | 1.5 | 43% |
| int4, RVV 4,096 bits | 419,551 | 4% | 1.6 | 50% |
| fp32 direct, RVV 128 bits | 7,643,299 | 17% | 7.0 | 81% |
| fp32 direct, RVV 4,096 bits | 7,646,338 | 12% | 5.0 | 97% |

(The floor is the bytes read per token at 25.6 GB/s and 2 GHz; reads in
flight are the total DRAM access latency over the run's time, by Little's
law; the stall share is the ROB's DRAM component of the CPI stack.)

(51 tokens: one prompt of 35 tokens prefilled in two batches, 16
decoded; every run reproduces the numpy model's tokens.  The DRAM
columns count 64-byte lines; the square kernels load a weight block once
per batch, 18 passes over the model in 51 tokens, 35.7 MB of fp32
weights per token and 4.5 MB of int4, the rest is the KV cache and
activations; the plain vector build runs the prefill token-serially and
streams the whole model for every token.  The writes are the
activations' evictions, 1 to 4 percent of the reads.)

These rows were measured after two memory-side corrections that every
earlier wide figure lacked (commit "Memory side"): a directory the size
of the L2 had written every clean line back to DRAM (writes equalled
reads) and cost a flush round trip per L2 miss, and the decoder had
delivered half the rows of each 32x32 square.  The fp32 square row was
6.8 million cycles per token with the scalar activations and attention;
the vector kernels for those took it to 4.7 million, and the int4 rows
from 13.8 and 13.4 million to 11.7 and 11.3 (all on the configuration
before 9.0).

9.0's configuration doubled the in-core fp32 square row, from 4.72 to
9.55 million cycles per token, on the same instructions and the same
DRAM lines.  Its square loads go through the L1-D, 64 lines each, and the
L1-D now has BOOM's eight MSHRs where it had no limit: with the limit
lifted the same trace runs at 6.05 million, and with the earlier core's
widths and queues as well (5-wide dispatch, a 48-entry load queue) at
4.69 million, the old figure.  The near-core tile loads its rows past
the L1-D and moved by 9 percent (4.36 to 4.73 million).  The same limit
moved the plain fp32 vector row from 32.1 to 44.8 million.

What the numbers say:

- **The bytes moved as promised.**  Against fp32 the int4 format
  streams 6.7x fewer bytes per token, and the counts are exact: the
  compressed format's DRAM traffic is what a hardware decompressor would
  move.  At 25.6 GB/s the fp32 stream alone is 2.9 million cycles per
  token, the int4 stream 0.42 million: that is the floor each format can
  reach, and the gap between them is the prize.
- **The int4 rows are bound by DRAM latency, not by instructions.**
  Cutting the kernel from 9.7 million to 1.7 million instructions per
  token (128 to 4,096 bits) gained 11 percent, and both rows sit 26 to 30x
  above their bandwidth floor.  The dot product walks one 256-byte weight
  row at a time, the configuration has no hardware prefetcher
  (`prefetcher = none` at every level of `config/riscv.cfg`), and the
  out-of-order window overlaps barely more than one 100 ns miss with the
  next (1.5 reads in flight): 84,000 line reads at 100 ns each is 16.8
  million cycles if they were serial, and the run takes 12.4 million.
- **Where the loads go decides the fp32 square rows.**  The near-core
  tile keeps 32 reads in flight and reaches 61 percent of the bandwidth,
  4.7 million cycles against the 2.9 million floor, and is the fastest
  row: the format that moves 6.7x more bytes is 2.6x faster than int4
  on the vector unit.  The same squares loaded through the core's L1-D
  queue for its eight MSHRs, keep 13 reads in flight and take twice as
  long.  The plain fp32 vector rows show the limit from the other side:
  the 4,096-bit unit, whose every weight load is a 32-line footprint,
  spends 97 percent of its cycles stalled on DRAM against 81 percent at
  128 bits on the same 78 million line reads, 5 reads in flight against
  7, and is 1.4x slower on the same bytes with 12x fewer instructions.
  A stream prefetcher or a DMA engine in front of the vector unit is the
  lever, and it is worth more than the instruction count: the
  accelerator form of the int4 kernel (the task path of section 3.4,
  streaming rows and dequantising inside the unit) would let the byte
  count become the time.
- **The core's own work is now under a million instructions per token.**
  With attention, normalisation and GELU on the vector unit the fp32
  square run issues 964,000 instructions per token (3.0 million before),
  and the gap to the bandwidth floor is the weight stream's latency, not
  the core: the accelerator form pairs with the compressed weights.

`tools/llm/wide-sweep.sh` reproduces every row; the harness case
`riscv-llm-wide` runs the two fast configurations and requires the DRAM
writes to stay a small fraction of the reads.
