# Matrix acceleration in Sniper: the components, how they attach to the core, how they are programmed

This document describes every piece that carries a matrix instruction
or an accelerator task from a specification to a simulated cycle count:
the software that generates and runs it, the hardware models that time
it, how each unit connects to the core, how a program drives it, and the
commands that build and run the examples.  It ends with the measurements
of the two workloads on every unit.  The decoder generation is described in
`RISCV_ISA_GENERATION.md`.

## 1. The path at a glance

```
 specification                 software                                  hardware model (Sniper)
 -------------                 --------                                  -----------------------
 riscv-unified-db (ratified)   tools/riscv/gen_isa.py + isa_policy.yaml  decoder_lib/riscv_isa_gen.*  (1436 insns, 38 classes)
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
`Q` the QEMU, `P` `build/release/lib/libsniper-qemu.so`,
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

All runs: Sniper with `config/riscv.cfg` (a single BOOM-class core at
2 GHz, L1 and a private L2), every count checked against what the
workload's shape implies (the harness cases of section 2.5).

**The LLM agent**, 379 tokens (276 prefill tokens in 16 batches of 8 to
26, 103 decoded), every kernel reproducing the numpy model's tokens:

| kernel | unit and coupling | cycles / token | instructions / token | vs RVV |
|---|---|---|---|---|
| scalar (F extension) | core | 683,660 | 1,161,416 | 0.20x |
| RVV 1.0, e32 m4 | core | 138,176 | 211,559 | 1.00x |
| IME 0.9, fp32 2x2 tiles | in-core | 294,099 | 452,697 | 0.47x |
| AME draft, fp32 4x4 squares | in-core (`ica`) | 131,273 | 169,709 | 1.05x |
| AME draft | attached tile, port at L2 (`oca_l2`) | 146,051 | 169,709 | 0.95x |
| AME draft | attached tile, near-core with transfer engine (`nca`) | 145,462 | 169,709 | 0.95x |
| DMA-attached accelerator, one task per weight product | tile datapath, port at L2 | 75,901 | 100,653 | 1.82x |
| DMA-attached accelerator | near-core | 75,901 | 100,653 | 1.82x |

The same kernels with the prefill token-serial, the shape measured
before the class B kernel existed: RVV 141,698, IME 472,610, AME
185,668 cycles per token.  Batching the prefill took IME from 3.3x to
2.1x slower than RVV and AME from 1.3x slower to 1.05x faster, with the
same instruction-level counts formulas confirming that every prefill
GEMM filled its tiles.

**gemm3 on the attached tile**, the 32x32x32 AME product repeated 16
times (`riscv-ame-couplings`): 97,360 tile ops, 69,632 rows, 8,192
squares under every coupling; cycles `ica` 384,400, `oca_l1` 410,400,
`oca_l2` 410,800, `oca_llc` 410,800 (the LLC is the L2 here), `nca`
411,000.

What the numbers say:

- **The matrix units need the matrix shape.**  Token-serial prefill
  gave both units one useful column per tile; the batched prefill fills
  them.  AME's 4x4 squares and 16 accumulators let a weight square be
  loaded once per prompt, which is why it now beats the vector unit
  even though decode (a quarter of the tokens) is still one column per
  square.  IME's 2x2 tiles at VLEN=128 carry too little work per
  instruction to catch up: 452,697 instructions per token against RVV's
  211,559.
- **The attached tile costs about 11 percent over the in-core cost on
  this workload**, from the dispatch latency and the in-order queue: the
  tile executes 45,876 ops per token in program order, and the ROB fills
  behind it.  Moving the port from L1 to L2 changed little, because the
  tile pipelines its rows and the out-of-order window covers their
  latency; the near-core transfer engine prefetched every row and gained
  0.4 percent for the same reason.  On the pure GEMM the spread between
  couplings is under 1 percent.  Where the port sits will matter for a
  workload that misses in L2, and for several units sharing it.
- **The DMA-attached accelerator is the fastest configuration**, 1.8x
  over RVV and 1.7x over AME in the core.  Per token: 13 tasks, 148,360
  multiply-accumulates, 9,275 cycles of compute at 16 per cycle, 3,190
  cycles of streaming, 196 KB streamed in (weights once per batch in
  prefill, once per token in decode), and 14,790 cycles waited at
  `acc.wait`.  The other 61,000 cycles per token are the core's own
  work between tasks: attention over the KV cache on the RVV dot
  product, layer normalisation, GELU, the embedding and cache writes.
  That is now the bottleneck, and it says what the next step is on this
  path: attention as a task or on the unit, and issuing the next
  independent task before waiting on the previous one (the kernel waits
  immediately today), which would recover part of the 14,790 cycles.
- **Near-core and L2 are identical for tasks**, because a task's blocks
  are streamed as a whole when the task starts; the transfer engine's
  prefetch-at-dispatch applies to instructions, not to tasks.
