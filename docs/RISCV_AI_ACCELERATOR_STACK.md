# The RISC-V, accelerator and AI-workload stack

Reference for everything on the RISC-V side of this tree: the generated
decoder, the patched QEMU and the frontend plugin, the vector and matrix
units in the core model, the attached tile and the DMA-attached
accelerator, the LLM agent workloads that drive them, and the tests that
hold each piece to an exact count.  The companion document
`VIRTUOSO_MIMICOS_STACK.md` covers the x86 side: the simulator itself,
its frontends, the MMU and MimicOS work, and the general regression
suites.

Every command in this document was run, in the order written, on the
commit named in the verification record at the end.  Commands sit in
`bash` blocks; the first block of section 4 sets the environment the
others assume.  Blocks whose first line starts with `# verify:` are the
ones the verification pass executes verbatim (`tools/doc-verify.py
DOC.md > run.sh` writes that pass as one script, `--long` the long
blocks).

---

## 1. What is here, in one page

| piece | where | what it does |
|---|---|---|
| Specification to decoder | `tools/riscv/gen_isa.py`, `tools/riscv/isa_policy.yaml`, `tools/riscv/overlay/` | Generates Sniper's RISC-V decoder, core-model class table, config block and an inline-assembly header from the RISC-V Unified Database (UDB) plus overlays: 1,436 instructions in 38 classes |
| AME overlay generator | `tools/riscv/gen_ame_udb.py` | Turns the Attached Matrix Extension draft (AsciiDoc source) into UDB YAML: 138 instructions under `Ztt` |
| Task-interface extension | `tools/riscv/overlay/.../Xsnacc` | Two instructions, `acc.task` and `acc.wait`, that hand a matrix product to a DMA-attached accelerator |
| Patched QEMU | `tools/riscv/qemu-11.1.1-ime.patch`, built into `qemu-riscv/` by `deps/sniper-deps --riscv-ime` | QEMU 11.1.1 riscv64 user mode with the IME 0.9 matrix instructions, the vector unit at the RVV 1.0 maximum of 65,536 bits, and four plugin-API additions for extensions emulated in a plugin |
| The frontend plugin | `frontend/qemu-plugin/` (`lib/libsniper-qemu.so`) | Records SIFT traces from QEMU, including the byte-exact memory footprint of every vector and matrix instruction; executes the AME and Xsnacc instructions on functional models inside the plugin |
| SIFT footprint records | `sift/` | `MemorySpans`: up to 32 byte ranges per instruction plus a count of cache lines beyond them |
| Core model | `common/performance_model/.../core_model_boom_v1.cc`, `config/riscv.cfg` | BOOM-class out-of-order core; every instruction class costed from the generated config block; a vector datapath width; the attached matrix tile with five couplings; the DMA-attached accelerator |
| Memory side | `common/core/memory_subsystem/` | Two corrections found by the 100 MiB workload: clean flushes no longer written to DRAM, and every row of a 32x32 footprint reaching the caches |
| Workloads | `test/riscv-gemm/`, `test/riscv-vlen-probe/`, `test/qemu-plugin-probe/`, `test/llm-agent/`, `test/llm-agent-wide/` | Self-checking matrix products, a vector-width probe, the plugin probes, and a tool-calling GPT agent with scalar, RVV, IME, AME, DMA-task and int4 kernels |
| Host tools | `tools/llm/` | The agent's trainer (numpy), the int4 quantizer, the 100 MiB sweep |
| Tests | `testing/test_riscv_isa.py`, the 21 `riscv-*` cases of `testing/test_qemu.py`, `tools/riscv/expected_counts.py`, `make riscv-ime-check` | Decoder drift, exact instruction-class and footprint counts, functional agreement with a Sail golden model, cycles per token |

The one command that runs the whole chain, from the specification to the
simulated agent:

```bash
make riscv-ime-check
```

It takes several hours (the LLM cases dominate); section 4.9 lists what it
runs.

---

## 2. The chain

```
 specification (UDB YAML)                             QEMU 11.1.1
   riscv-unified-db  +  IME 0.9 overlay                 + tools/riscv/qemu-11.1.1-ime.patch
   + tools/riscv/overlay (vsetlambda, Ztt, Xsnacc)        IME instructions, VLEN to 65,536,
        |                                                  plugin-API additions
        v                                                       |
 tools/riscv/gen_isa.py + isa_policy.yaml                       |
   decoder_lib/riscv_isa_gen.{h,cc}   table, classes             |
   include/riscv_insn.h               .insn macros -----> guest built with a stock clang
   config/riscv*.cfg                  class costs               |
        |                                                       v
        |                                  frontend/qemu-plugin (libsniper-qemu.so)
        |                                    branch and footprint flags from the same tables
        |                                    AME and Xsnacc models (claimed at translation)
        |                                    SIFT trace with MemorySpans
        v                                                       |
 Sniper: riscv_decoder -> core_model_boom_v1 (class costs, vector datapath)
         trace_thread replays every footprint span as a sized cache access
         rob_timer -> AmeTile (coupling ica|oca_l1|oca_l2|oca_llc|nca; DMA tasks)
         statistics riscv.<class>, riscv.spans, ame_tile.*
```

Nothing on the workload path is rebuilt when a specification changes: the
guest is compiled by an unmodified clang through the generated header, QEMU
is patched once, and every stage past QEMU learns the ISA from the same
YAML.

---

## 3. Components

### 3.1 Specification to decoder

`tools/riscv/gen_isa.py` reads one or more UDB roots (later roots override
earlier ones) and the policy file, and writes four things: the instruction
and class tables (`decoder_lib/riscv_isa_gen.{h,cc}`), the inline-assembly
header (`include/riscv_insn.h`, with `--header`), the generated block of
each named config (`--config`), and, with `--report`, a listing of class
membership and encoding overlaps.  The generated files carry the UDB
commit they came from in their first comment.

`tools/riscv/isa_policy.yaml` is what the specification does not say:

| section | content |
|---|---|
| `xlen` | 64 |
| `decoder` | `vector_bytes: 16` (the static size of one vector memory micro-op), `memory_uops: 32` (how many micro-ops a footprint instruction may spread over) |
| `header` | which extensions get `.insn` macros: `^Zvv` (IME), `^Ztt` (AME), `^Xsnacc`; prefix `RISCV_` |
| `extensions` | regexes selecting what to include from the UDB (base, compressed, atomic, bit manipulation, FP, vector, privileged, plus the three overlays) |
| `classes` | ordered rules mapping instructions to the 38 classes with a placeholder latency, issue interval and CPI group each; vector loads and stores are told apart by their assembly template, not their mnemonic |
| `accumulates` | forms whose destination is also a source (multiply-accumulates, reductions) |
| `mem_size` | bytes per scalar memory instruction |

The overlays in `tools/riscv/overlay/` follow the UDB layout:

| directory | instructions |
|---|---|
| `Zvvmm/` | `vsetlambda`, the one IME instruction the upstream contribution does not define |
| `Ztt/` | the whole AME draft, 138 instructions, generated by `tools/riscv/gen_ame_udb.py` from the working group's AsciiDoc source (encodings on the CUSTOM-1 opcode, provisional) |
| `Xsnacc/` | `acc.task xs1` and `acc.wait` (CUSTOM-3, funct3 0, funct7 0 and 1), classes `acc_task` and `acc_wait`, both footprint instructions |

`decoder_lib/riscv_isa.{h,cc}` is the API over the tables (decode,
operands, disassembly, self-test); `decoder_lib/riscv_decoder.{h,cc}` is
Sniper's decoder built on it.  A footprint instruction (any vector or
matrix memory instruction) gets `memory_uops` load or store micro-ops
statically; each recorded byte range takes one and the rest are squashed.

`testing/test_riscv_isa.py` keeps the tables honest: with
`SNIPER_UDB_ROOTS` set it regenerates into a scratch directory and requires
byte-identical output; without it, it still compiles the tables alone and
decodes twenty known encodings to their expected name and class.

### 3.2 QEMU: the patch

`deps/sniper-deps --riscv-ime` clones QEMU 11.1.1, applies
`tools/riscv/qemu-11.1.1-ime.patch` and builds riscv64 user mode with TCG
plugins into `qemu-riscv/build/qemu-riscv64`, once per version into the
shared dependency cache.  The patch (28 files) carries three things:

- **IME 0.9 (Zvvm)**: the tile load/store and matrix multiply-accumulate
  helpers and `vsetlambda`, from the IME pipeline's generator, behind the
  CPU properties `x-zvvmm`, `x-zvvfmm` (fp), `x-zvvmtls`, `x-zvvmttls`
  (default off).
- **The vector unit to the RVV 1.0 maximum.**  Stock QEMU stops at 1,024
  bits.  Four fixes take it to 65,536: the cap itself with a 32-bit `vlen`
  property; the generic-vector descriptor's size field (8 to 13 bits:
  operands above 2 KB were silently truncated, so simple integer ops on a
  full m8 group came out wrong); the unit-stride and whole-register
  load/store helpers looping over pages instead of assuming two; and the
  tile helper buffers on the heap, sized for a whole group.
  `test/riscv-vlen-probe` found each of them and checks every VLEN.
- **Four plugin-API additions** (section 3.3).

The CPU string every example uses:
`-cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on`; `vlen` takes any
power of two up to 65536.  `tools/riscv/regen-qemu-patch.sh <pristine>
<patched>` regenerates the patch from the two trees (`qemu-src/` is the
pristine checkout the fetcher keeps) and checks that it applies.

### 3.3 The frontend plugin

`frontend/qemu-plugin/sniper_qemu.cc` is a TCG plugin loaded into QEMU
with `-plugin lib/libsniper-qemu.so,<options>`.  Options:

| option | meaning |
|---|---|
| `output=<prefix>` | trace files `<prefix>.app0.th<n>.sift` |
| `flow=N` | instructions per quantum (user-mode flow control) |
| `response` | blocking flow control for single-threaded user-mode guests |
| `time`, `ns=N` | system-mode time control (x86 full system) |
| `ame_n=<N>` | the AME square side the model uses: 4 by default, up to 32 |
| `claim=0` | emulate through the illegal-instruction path even when QEMU can claim (comparison) |
| `trace=0` | a functional run: the models only, no trace file |
| `verbose` | per-vCPU counts |

For RISC-V the plugin decodes each instruction once per translation with
the generated tables, so it knows which instructions are conditional
branches and which carry a footprint.  For a footprint instruction it
records every element access the vector unit makes, coalesces contiguous
runs into byte ranges, and writes them ahead of the instruction as a
`MemorySpans` record.  When a footprint spans more than 32 runs (a gather
over thousands of lines at a wide VLEN) it coalesces by cache line, keeps
the first 32 lines and counts the rest, so the trace still says how much
the instruction touched.

The AME and Xsnacc instructions have no QEMU implementation.  The plugin
executes them on functional models: `ame_backend.{h,cc}` (M registers,
accumulators, datatype registers, ownership, Int32 and FP32 squares of side
`ame_n`) and `acc_backend.{h,cc}` (`acc.task` reads a 48-byte descriptor
and computes `Y = X W^T + bias`, `acc.wait` delivers `Y`).  Each memory
access the model makes goes into the pending footprint, so the simulator
sees the unit's traffic.

The four plugin-API additions the patch makes to QEMU, and what the plugin
does with them:

| call | effect |
|---|---|
| `qemu_plugin_register_vcpu_insn_claim_cb` | the RISC-V translator offers every instruction it cannot decode to the plugins before it raises illegal-instruction; the plugin claims the AME and Xsnacc instructions, registers its callback on them, and the block continues through them (no exception, no loop exit, no block lookup) |
| `qemu_plugin_raise_exception(vaddr, cause, tval)` | the models raise the draft's exceptions as real ones: illegal instruction for use without ownership or a bad datatype, a load or store access fault for a square row at an unmapped address; user mode delivers them as SIGILL and SIGSEGV |
| `qemu_plugin_register_vcpu_registers` | the unit's seven CSRs, its datatype registers and its M and accumulator squares (103 registers) join the vcpu's gdb register list under `org.riscv.ame`: gdb, `qemu_plugin_read_register` and other plugins see them |
| `qemu_plugin_register_sysreg_cb` | `csrr`/`csrw` of `amenlen`, `ameudsz`, `amestype`, `ameown`, `amefflags`, `amexsat` and `amestatus` reach the model instead of trapping |

The plugin binds the four weakly (`qemu_plugin_ext.h`): loaded into a stock
QEMU (the x86 frontend) it takes the older exception-and-set-PC path.  The
exit summary counts both: `plugin-emulated instructions: N claimed at
translation, M through the exception path`.

### 3.4 SIFT: spans and dropped lines

`Sift::Writer::MemorySpans(count, spans, dropped_lines)` writes a record of
up to `MAX_MEMORY_SPANS = 32` byte ranges (address, size, direction) plus a
count of cache lines the instruction touched beyond them.  The reader
accepts records with or without the count, so older traces still load.
The trace thread replays each span as one sized cache access (the core
splits it over lines) and counts `riscv.spans`, `riscv.span_bytes` and
`riscv.spans_dropped_lines`.  `siftdump` prints the spans of a trace.

### 3.5 The core model

`config/riscv.cfg` (and `riscv-mediumboom.cfg`) select `core_model =
riscv` on the `rob` timer with the geometry of BOOM v3's MegaBoomConfig
(4-wide, 128-entry ROB, 32-entry load and store queues, 32 KiB L1 caches,
TAGE) and MediumBoomConfig (2-wide, 64-entry ROB, 16 KiB L1 caches, 1 GHz)
respectively, a 1 MiB L2 shared by a cluster of up to four cores (a run of
fewer has it to itself) with a directory slice at each L2, 4 GiB of MimicOS
memory, Sv39 translation
(`pagetable_configs/3_level_radix_sv39.cfg`) and a directory sized above the
caches.  Before 9.0 both files carried the Cortex-A72's core parameters.  Everything below the `# BEGIN generated`
marker is written by `gen_isa.py`: `[perf_model/core/riscv]` with
`vector_bytes`, `memory_uops`, one `<class>_latency` per class and an
`_issue_cycles` for the matrix classes.  Hand-written sections sit above
the marker:

| section | keys |
|---|---|
| `[perf_model/core/vector]` | `datapath_bytes` (0: a whole register group per cycle, the placeholder; 64: a vector instruction holds its port for the cycles its group takes through a 64-byte datapath), `lmul` (the group size assumed) |
| `[perf_model/ame_tile]` | `coupling` (`ica`, `oca_l1`, `oca_l2`, `oca_llc`, `nca`), `dispatch_latency`, `result_latency`, `bytes_per_cycle`, `mma_issue_cycles`, `mma_macs_per_cycle`, `mma_latency`, `other_latency`, `macs_per_cycle` (the DMA accelerator's rate) |
| `[perf_model/dram]` | `per_controller_bandwidth = 25.6` GB/s: one DDR4-3200 channel, the edge system the studies assume (`base.cfg`'s 5 is a placeholder); `tools/llm/wide-sweep.sh` sweeps it |

**The attached tile** (`rob_performance_model/ame_tile.{h,cc}`): with a
coupling other than `ica`, the AME and Xsnacc micro-ops stay in the core's
ROB (so the M and accumulator registers give real dependencies) but
execute on an in-order tile with its own throughput and its own memory
port, entering the cache hierarchy at the level the coupling names;
`nca` adds a transfer engine that fetches a square's rows at dispatch.
The tile infers the square side from the rows it loads.  An `acc.task`
streams its blocks through the port, computes at `macs_per_cycle`, and
`acc.wait` holds the ROB until the output is written.  Statistics under
`ame_tile.*`: `ops`, `rows`, `bytes`, `mma`, `prefetch_hits`, `busy`,
`queue_wait`, `idle`, `mem_latency`, and for tasks `tasks`,
`task_bytes_in`, `task_bytes_out`, `task_macs`, `task_dma`,
`task_compute`, `task_wait`.

**Memory-side corrections** made while measuring the 100 MiB agent, both
covered by exact checks now: the directory had as many entries as the L2
has lines, so it dropped an entry on every fill and wrote every flushed
clean line to DRAM (writes equalled reads; a flush round trip per miss);
the flush reply now says whether the data was modified and the RISC-V
configs carry a 65,536-entry directory.  And the decoder capped a
footprint at 16 micro-ops while the trace carries 32, so every 32x32
square reached the caches with half its rows; the cap is 32 and the
32x32 case checks the tile's rows and bytes exactly.

### 3.6 Workloads

All guests are freestanding (`-ffreestanding -nostdlib -static`, entered at
`_start`), built by a stock clang with `-march=rv64gcv`; the matrix
instructions come from `include/riscv_insn.h`.

| workload | what it checks | exit code |
|---|---|---|
| `test/qemu-plugin-probe/rv-loop.c` | a counted loop of exactly 100,007 instructions | 0 |
| `test/qemu-plugin-probe/rv-ime.c` | one 2x2 IME product on tiles, 33 instructions, 4 footprints | 100 |
| `test/riscv-gemm/gemm3.c` | one int32 `C = A x B^T` four ways: scalar, RVV, IME, AME (`-DUSE_AME`); `-DM/-DN/-DK`, `-DAME_N`, `-DIME_VLEN/-DIME_LMUL/-DIME_LAMBDA`, `-DAME_REPEAT`; a Sail variant with `-DPLATFORM_HTIF -DNO_IME -Wl,-T,sail.ld` | 100 when all agree; 4 + a mask (1 RVV, 2 IME, 4 AME) |
| `test/riscv-vlen-probe/vlen_probe.c` | 14 instruction families on a full register group at the CPU's VLEN against a scalar reference | 100, or 10 x family + step |
| `test/riscv-vlen-probe/gather.c` | indexed loads over more lines than a record holds | 100, or 10 + gather |
| `test/qemu-plugin-probe/rv-ame-api.c` | the plugin-API additions: CSRs, ownership, a claimed product (`-DMODE=0`), use without ownership (`-DMODE=1`, SIGILL), an unmapped square (`-DMODE=2`, SIGSEGV) | 100, or 3..12 |
| `test/qemu-plugin-probe/rv-ame-loop.c` | 4 million `ame.release`, to time the claim against the exception path | 100 |
| `test/llm-agent/agent.c` | the tool-calling GPT agent (below) | 100; 5 a line differs from numpy; 6 answers wrong; 7 both |

**The LLM agent.** `tools/llm/train_agent_gpt.py` trains, on the host in
numpy, a character-level GPT that answers arithmetic questions by calling
tools (`add`, `mul`, `len`) in a `Q:`/`T:`/`R:`/`A:` protocol, and exports
`model_weights.h` (or a binary blob with `--blob`) and `prompts.h`, the
numpy model's own outputs.  `agent.c` runs the same model on RISC-V with
a KV cache and greedy decoding, batches queued prompt text as one prefill
GEMM, and compares every generated line with numpy.  Kernels are chosen
at compile time:

| flag | weight products | everything else |
|---|---|---|
| (none) | scalar | scalar |
| `-DUSE_RVV` | RVV dot product (e32, m4) | layer normalisation, GELU and attention on the vector unit |
| `-DUSE_IME` | IME tiles (`-DIME_VLEN -DIME_LMUL -DIME_LAMBDA`; 2x2 by default, 32x32 at 4096/8/4) | as RVV |
| `-DUSE_AME` | AME squares (`-DAME_N`, 4 by default, 32) | as RVV |
| `-DUSE_DMA` | one `acc.task` per weight product | as RVV |
| `-DUSE_W4` | int4 weights dequantised inside the RVV dot product (`tools/llm/quantize_w4.py`) | as RVV |

The guest reports `tokens: P prefilled, G generated`, the prefill batch
sizes, its attention dot and softmax-strip counts and (int4) its matvec
count, from which the harness derives every matrix-unit and vector-class
instruction count exactly.  The small model (d = 64, 3 layers, 40/40 fresh
questions right after 6,000 steps) is committed; the 100 MiB model
(d = 512, 8 layers, untrained, `test/llm-agent-wide/`) is generated in a
minute and swept by `tools/llm/wide-sweep.sh`.

### 3.7 Tests

| test | what it holds |
|---|---|
| `testing/test_riscv_isa.py` | the generated tables match a fresh generation (with `SNIPER_UDB_ROOTS`) and decode 20 known encodings |
| `tools/riscv/expected_counts.py` | gemm3's per-class counts and footprint bytes, derived from the specification's semantics |
| `riscv-user-trace`, `riscv-user-simulate` | 100,007 instructions traced and simulated, branches agree with the decoder |
| `riscv-ime-trace`, `riscv-ime-simulate` | every tile row recorded and seen by the cache |
| `riscv-gemm-verify`, `riscv-gemm-simulate`, `riscv-gemm-sail` | scalar, RVV, IME agree; counts exact; RVV agrees with the Sail golden model |
| `riscv-ame-verify`, `riscv-ame-simulate` | AME on the plugin model agrees; strided-square footprints exact |
| `riscv-ame-couplings` | the tile under every coupling on a 32-cubed product: tile counts exact, timing responds |
| `riscv-size32-verify` | gemm3 at 64x64x64 on 32x32 tiles and squares |
| `riscv-vlen-probe` | every family at every VLEN from 128 to 65,536 |
| `riscv-footprint-lines` | recorded spans plus dropped lines equal the lines a wide gather touched |
| `riscv-vector-datapath` | a 16-byte datapath is slower than the full width, counts identical |
| `riscv-llm-agent` | six kernels answer every prompt as numpy |
| `riscv-llm-simulate` | RVV, IME, AME, int4 cycles per token; matrix, int4 and vector-class counts exact |
| `riscv-llm-couplings` | AME on the tile and the DMA accelerator under `oca_l2` and `nca` |
| `riscv-llm-size32` | 32x32 units and a 4,096-bit vector unit; rows and bytes exact |
| `riscv-llm-vlen` | the vector-length-agnostic kernels at 256, 4,096 and 65,536 bits |
| `riscv-llm-wide` | the 100 MiB agent on the two fast configurations; DRAM writes a small fraction of reads |
| `riscv-plugin-api` | claimed and trapped counts, the two signals, the CSRs, the registers over the gdb protocol |

---

## 4. Running it

### 4.1 Environment

Prerequisites: the simulator built with `RISCV=ON QEMU=ON RISCV_IME=ON`
(section 4.2), a clang of version 15 or later with a riscv64 target and
lld (no rebuilt compiler: distribution packages work), and, for the
decoder regeneration only, checkouts of the RISC-V Unified Database and
the IME 0.9 pipeline's UDB overlay.  Every block below assumes this one
was run first in the same shell, from the repository root:

```bash
# verify:env
export SNIPER_ROOT=$(pwd)
export SNIPER_RISCV_CC=${SNIPER_RISCV_CC:-clang-15}             # any clang >= 15 with a riscv64 target and lld
export SNIPER_QEMU_RISCV=$SNIPER_ROOT/qemu-riscv/build/qemu-riscv64
# Recordings made by hand are laid out as record-trace lays them out, within
# Sv39's 2^38 (-R 0x4000000000): the riscv configuration's page table refuses an
# address above it.
export PLUGIN=$SNIPER_ROOT/build/default/lib/libsniper-qemu.so
export RVCPU=max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on   # the IME features on; vlen up to 65536
WORK=${WORK:-/tmp/sniper-riscv-examples}; mkdir -p $WORK
rvcc() { $SNIPER_RISCV_CC --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding \
         -nostdlib -static -fuse-ld=lld -Wl,-e,_start -I include "$@"; }
```

### 4.2 Build

```bash
# verify:build
make -j$(nproc) RISCV=ON QEMU=ON RISCV_IME=ON      # deps (QEMU x86 + patched riscv64, Sail), configure, build
python3 deps/sniper-deps --qemu --riscv-ime status   # what the fetcher holds for this feature set
$SNIPER_QEMU_RISCV --version | head -1
ls -l build/default/lib/libsniper-qemu.so lib/sniper
```

### 4.3 The decoder: drift check, regeneration, the AME overlay

The checked-in tables are always checked; regeneration needs the UDB
roots.

```bash
# verify:decoder
python3 testing/test_riscv_isa.py                     # tables: compiles the decoder alone, decodes 20 known encodings
SNIPER_UDB_ROOTS="$UDB_UPSTREAM;$UDB_IME" python3 testing/test_riscv_isa.py   # regen: byte-identical to a fresh generation
python3 tools/riscv/gen_isa.py --udb $UDB_UPSTREAM --udb $UDB_IME --udb tools/riscv/overlay \
        --policy tools/riscv/isa_policy.yaml --out $WORK/decoder --report > $WORK/decoder-report.txt
head -12 $WORK/decoder-report.txt                                    # class membership, then overlaps and skipped instructions
diff -q $WORK/decoder/riscv_isa_gen.cc decoder_lib/riscv_isa_gen.cc && echo "generated tables identical"
python3 tools/riscv/gen_ame_udb.py --ame $AME_SPEC --out $WORK/overlay && ls $WORK/overlay/spec/std/isa/inst/Ztt | wc -l
```

(`UDB_UPSTREAM` and `UDB_IME` are the two UDB checkouts, `AME_SPEC` the
`riscv-attached-matrix-extension` checkout; the verification record names
the revisions used.)  To adopt a new specification: drop it in, run the
`gen_isa.py` line with `--out decoder_lib --config
config/common_configs/riscv_isa_classes.cfg --header include/riscv_insn.h`,
rebuild, and run the drift check.

### 4.4 gemm3: four ways, traced, simulated, counted, and on the Sail model

```bash
# verify:gemm3
rvcc -I test/riscv-gemm -DUSE_AME -o $WORK/gemm3 test/riscv-gemm/gemm3.c
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/gemm3 -- $WORK/gemm3; echo "exit $? (100 = scalar, RVV, IME and AME agree)"
build/default/lib/siftdump $WORK/gemm3.app0.th0.sift | grep -c -- '-- spans'      # footprint records in the trace
./run-sniper -n 1 -c riscv -d $WORK/gemm3-sim --traces=$WORK/gemm3.app0.th0.sift > /dev/null
python3 tools/riscv/expected_counts.py $WORK/gemm3-sim/simulation/sim.stats --ame   # every class count and footprint byte exact
grep -E '^riscv\.(v_load|ime_mma_int|ame_mma|span_bytes) ' $WORK/gemm3-sim/simulation/sim.stats
```

The same product at 32x32 (64x64x64 so the units are filled), and on the
Sail golden model (scalar and RVV only, since Sail has no IME):

```bash
# verify:gemm3-32-sail
rvcc -I test/riscv-gemm -DUSE_AME -DM=64 -DN=64 -DK=64 -DAME_N=32 -DIME_VLEN=4096 -DIME_LMUL=8 -DIME_LAMBDA=4 \
     -o $WORK/gemm3-32 test/riscv-gemm/gemm3.c
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu ${RVCPU/vlen=128/vlen=4096} -plugin $PLUGIN,output=$WORK/gemm3-32,ame_n=32 -- $WORK/gemm3-32; echo "exit $?"
rvcc -I test/riscv-gemm -DPLATFORM_HTIF -DNO_IME -Wl,-T,test/riscv-gemm/sail.ld -o $WORK/gemm3-sail test/riscv-gemm/gemm3.c
echo '{"extensions": {"V": {"vlen_exp": 7}}}' > $WORK/sail-v128.json      # the model's VLEN: 2^7 = 128 bits
sail-riscv/bin/sail_riscv_sim --config-override $WORK/sail-v128.json $WORK/gemm3-sail 2>&1 | tail -2
```

### 4.5 The LLM agent

Every kernel, functionally, under QEMU:

```bash
# verify:agent-kernels
for v in scalar rvv ime ame dma w4; do
   f=; [ $v = scalar ] || f=-DUSE_$(echo $v | tr a-z A-Z)
   rvcc -I test/llm-agent $f -o $WORK/agent-$v test/llm-agent/agent.c
done
$SNIPER_QEMU_RISCV -cpu $RVCPU $WORK/agent-rvv | tail -4; echo "rvv exit ${PIPESTATUS[0]} (100 = every line as numpy, every answer right)"
$SNIPER_QEMU_RISCV -cpu $RVCPU $WORK/agent-ime > /dev/null; echo "ime exit $?"
$SNIPER_QEMU_RISCV -cpu $RVCPU $WORK/agent-w4 > /dev/null; echo "w4 exit $?"
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,trace=0 -- $WORK/agent-ame 2>&1 | tail -1; echo "ame (plugin model, functional run)"
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,trace=0 -- $WORK/agent-dma > /dev/null; echo "dma exit $?"
```

The vector kernels are vector-length agnostic; the RVV agent at the
maximum VLEN:

```bash
# verify:agent-vlen
$SNIPER_QEMU_RISCV -cpu ${RVCPU/vlen=128/vlen=65536} $WORK/agent-rvv > /dev/null; echo "rvv at 65,536 bits: exit $?"
```

Traced and simulated, with the per-token cost and the exact counts:

```bash
# verify:agent-simulate
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/agent-rvv -- $WORK/agent-rvv > $WORK/agent-rvv.out; grep -E '^tokens|^attention' $WORK/agent-rvv.out
./run-sniper -n 1 -c riscv -d $WORK/agent-rvv-sim --traces=$WORK/agent-rvv.app0.th0.sift > /dev/null
python3 - <<'EOF'
import re
st = dict(l.split(' = ', 1) for l in open('/tmp/sniper-riscv-examples/agent-rvv-sim/simulation/sim.stats') if ' = ' in l)
m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', open('/tmp/sniper-riscv-examples/agent-rvv.out').read())
tokens = int(m.group(1)) + int(m.group(2))
print('cycles/token %.0f  insn/token %.0f  v_fp_muladd %s' % (float(st['performance_model.cycle_count']) / tokens,
      int(st['performance_model.instruction_count']) / tokens, st['riscv.v_fp_muladd'].split(',')[0]))
EOF
```

AME on the attached tile under two couplings, and the DMA accelerator at
two rates (the tile's statistics show the work moved off the core):

```bash
# verify:agent-tile
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/agent-ame -- $WORK/agent-ame > /dev/null
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/agent-dma -- $WORK/agent-dma > /dev/null
for c in oca_l2 nca; do
   ./run-sniper -n 1 -c riscv -d $WORK/ame-$c -g perf_model/ame_tile/coupling=$c --traces=$WORK/agent-ame.app0.th0.sift > /dev/null
   echo "ame/$c: $(grep -E '^performance_model.cycle_count|^ame_tile.mma ' $WORK/ame-$c/simulation/sim.stats | tr '\n' ' ')"
done
for macs in 16 1024; do
   ./run-sniper -n 1 -c riscv -d $WORK/dma-$macs -g perf_model/ame_tile/coupling=nca -g perf_model/ame_tile/macs_per_cycle=$macs --traces=$WORK/agent-dma.app0.th0.sift > /dev/null
   echo "dma at $macs MACs/cycle: $(grep -E '^performance_model.cycle_count|^ame_tile.tasks |^ame_tile.task_macs ' $WORK/dma-$macs/simulation/sim.stats | tr '\n' ' ')"
done
```

The 32x32 geometry (a 4,096-bit vector unit, 32x32 IME tiles and AME
squares), functionally and simulated in the core:

```bash
# verify:agent-32
rvcc -I test/llm-agent -DUSE_AME -DAME_N=32 -o $WORK/agent-ame32 test/llm-agent/agent.c
rvcc -I test/llm-agent -DUSE_IME -DIME_VLEN=4096 -DIME_LMUL=8 -DIME_LAMBDA=4 -o $WORK/agent-ime32 test/llm-agent/agent.c
CPU4K=${RVCPU/vlen=128/vlen=4096}
$SNIPER_QEMU_RISCV -cpu $CPU4K $WORK/agent-ime32 > /dev/null; echo "ime32 exit $?"
$SNIPER_QEMU_RISCV -cpu $CPU4K -plugin $PLUGIN,output=$WORK/agent-ame32,ame_n=32 -- $WORK/agent-ame32 > /dev/null; echo "ame32 exit $?"
./run-sniper -n 1 -c riscv -d $WORK/ame32-sim -g perf_model/core/riscv/ame_mma_issue_cycles=32 --traces=$WORK/agent-ame32.app0.th0.sift > /dev/null
grep -E '^performance_model.cycle_count|^riscv.ame_mma ' $WORK/ame32-sim/simulation/sim.stats
```

The int4 quantizer regenerates the committed small-model files (the
result must be identical to what is committed):

```bash
# verify:quantize
python3 tools/llm/quantize_w4.py | tail -2
git status --short test/llm-agent/ | grep -c . || echo "quantizer output identical to the committed files"
```

Training a model of your own (the committed one took 6,000 steps, about
an hour; a few hundred steps already answer most questions):

```bash
# verify:train
python3 tools/llm/train_agent_gpt.py --out $WORK/model-300 --steps 300 --eval-questions 10 --prompts 4 | tail -3
cp test/llm-agent/agent.c $WORK/model-300/agent.c      # a quoted #include finds the model next to the source
rvcc -I $WORK/model-300 -DUSE_RVV -o $WORK/agent-300 $WORK/model-300/agent.c
$SNIPER_QEMU_RISCV -cpu $RVCPU $WORK/agent-300 | tail -2; echo "exit ${PIPESTATUS[0]} (6 = every line as numpy, some answers wrong: expected for a short training)"
```

The 100 MiB agent: generate it (untrained; the numpy model's own outputs
are the reference), then sweep the fast configurations.  Each
configuration records about 150 million instructions and simulates for
half an hour to an hour; `fp32-rvv` alone takes hours.

```bash
# verify:long:wide
python3 tools/llm/train_agent_gpt.py --out test/llm-agent-wide --d 512 --layers 8 --ctx 128 \
        --steps 0 --export-anyway --eval-questions 2 --prompts 1 --gen 8 --blob | tail -1
python3 tools/llm/quantize_w4.py --dir test/llm-agent-wide --questions 1 --prompts 1 --export-anyway | tail -1
tools/llm/wide-sweep.sh $WORK/wide 25.6 fp32-ame32 fp32-ame32-nca w4-rvv4k
```

### 4.6 The vector unit at every width

```bash
# verify:vlen
rvcc -o $WORK/vlen_probe test/riscv-vlen-probe/vlen_probe.c
for vl in 128 1024 4096 65536; do $SNIPER_QEMU_RISCV -cpu ${RVCPU/vlen=128/vlen=$vl} $WORK/vlen_probe; echo "VLEN $vl: exit $? (100 = every family agrees)"; done
rvcc -o $WORK/gather test/riscv-vlen-probe/gather.c
$SNIPER_QEMU_RISCV -cpu ${RVCPU/vlen=128/vlen=65536} -plugin $PLUGIN,output=$WORK/gather -- $WORK/gather 2>&1 | grep -E 'footprints|beyond the spans'
```

### 4.7 The plugin-API additions

```bash
# verify:plugin-api
for m in 0 1 2; do rvcc -DMODE=$m -o $WORK/rv-ame-api$m test/qemu-plugin-probe/rv-ame-api.c; done
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/api -- $WORK/rv-ame-api0 2>&1 | grep -E 'AME registers|plugin-emulated'; echo "mode 0: exit ${PIPESTATUS[0]} (100: CSRs, ownership and a claimed product all check)"
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/api,claim=0 -- $WORK/rv-ame-api0 2>&1 | grep plugin-emulated
$SNIPER_QEMU_RISCV -cpu $RVCPU $WORK/rv-ame-api0 > /dev/null 2>&1; echo "without the plugin: exit $? (132 = SIGILL, QEMU does not know the CSRs)"
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/api -- $WORK/rv-ame-api1 2>&1 | grep 'AME\] error'; echo "mode 1: exit ${PIPESTATUS[0]} (132 = the model raised illegal-instruction)"
$SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,output=$WORK/api -- $WORK/rv-ame-api2 2>&1 | grep 'AME\] error'; echo "mode 2: exit ${PIPESTATUS[0]} (139 = the model raised a load access fault)"
rvcc -o $WORK/rv-ame-loop test/qemu-plugin-probe/rv-ame-loop.c
time $SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,trace=0 -- $WORK/rv-ame-loop
time $SNIPER_QEMU_RISCV -R 0x4000000000 -cpu $RVCPU -plugin $PLUGIN,trace=0,claim=0 -- $WORK/rv-ame-loop
[ $? -eq 100 ] && echo "rv-ame-loop: exit 100, the probe's success code"
```

The unit's registers over the gdb protocol (any gdb with RISC-V support
can `target remote :4711` and `info registers amenlen`; here a small
client reads the target description and three registers):

```bash
# verify:gdb
$SNIPER_QEMU_RISCV -cpu $RVCPU -g 4711 -plugin $PLUGIN,output=$WORK/apigdb -- $WORK/rv-ame-api0 > /dev/null 2>&1 &
python3 - <<'EOF'
import sys; sys.path.insert(0, 'testing'); import test_qemu
feats, regs, values = test_qemu.gdb_read_feature(4711, 'org.riscv.ame', ('amenlen', 'ameudsz', 'ameown'))
print('org.riscv.ame.xml' in feats, len(regs), 'registers;', {k: v for k, v in values.items()})
EOF
wait
```

### 4.8 Regenerating the QEMU patch

After editing the patched tree, the patch is rewritten from the pristine
and patched checkouts and checked to apply:

```bash
# verify:patch
tools/riscv/regen-qemu-patch.sh qemu-src qemu-riscv && git diff --stat tools/riscv/qemu-11.1.1-ime.patch | tail -1
```

### 4.9 The harness

Individual cases, and the whole chain:

```bash
# verify:harness
python3 testing/test_qemu.py --case riscv-user-trace --case riscv-user-simulate --case riscv-ime-simulate \
        --case riscv-gemm-verify --case riscv-gemm-simulate --case riscv-gemm-sail --case riscv-ame-verify \
        --case riscv-ame-simulate --case riscv-size32-verify --case riscv-vlen-probe --case riscv-plugin-api
```

```bash
# verify:long:ime-check
make riscv-ime-check      # steps 1-5: decoder drift, build, QEMU, compiler, every riscv-* case
```

The full harness (`python3 testing/test_qemu.py`, 46 cases: 27 RISC-V, 5
AArch64, the x86-64 user and system cases, and the run-sniper front-end cases)
is the gate every commit passed.

### 4.10 Address translation: Sv39

Every configuration models address translation (`general/translation_enabled`
in `config/base.cfg`), and until now every one of them built the x86-64 page
table: four levels over a 48-bit space.  The riscv configurations now include
`config/pagetable_configs/3_level_radix_sv39.cfg`: Sv39, three levels of 512
entries over a 39-bit space, 4 KiB pages and 2 MiB megapages, two page walk
caches.  The radix table takes any level count (each level's index is the nine
bits above the level below it; `radix_4level` is unchanged for x86-64 and is
also AArch64's 4 KiB-granule geometry, which the cortex configurations note).

A guest has to live inside the 39-bit space.  QEMU user mode lays a riscv64
guest out over 48 bits (its stack near 2^47), so `record-trace` confines it
with `-R 0x4000000000` by default; `run-sniper --qemu-va-bits=48` keeps QEMU's
layout, for Sv48 with `pagetable_configs/4_level_radix_sv48.cfg`.  An address
above the configured space is refused by the simulator -- "does not fit the
39-bit virtual address space of page table radix_sv39" -- rather than indexing
the root with the wrong bits.

`testing/apps/touch_pages.c` touches 512 pages in 8 regions of 2 MiB, four
pages of its stack and one of code, and witnesses the walk depth through the
page-table frames the kernel allocates:

| run | faults | walks | page-table frames |
|---|---|---|---|
| RISC-V, Sv39 (`-c riscv`) | 517 | 1034 | 12 |
| RISC-V, Sv48 (`-c riscv -c pagetable_configs/4_level_radix_sv48 --qemu-va-bits=48`) | 517 | 1034 | 14: one level more, two tables |
| RISC-V, 48-bit layout against Sv39 (`-c riscv --qemu-va-bits=48`) | refused | | |
| AArch64, `-c cortex-a72` (4 levels) | 518 | 1036 | 14 |

```bash
# verify:translation
python3 testing/test_qemu.py --case riscv-user-translation --case aarch64-user-translation
```

---

## 5. Results

Cycles per token on the small model (379 tokens, 25.6 GB/s), every count
exact; the full tables and their reading are in
`ACCELERATOR_COMPONENTS.md`.

| kernel | unit | cycles / token |
|---|---|---|
| scalar | core | 683,660 |
| RVV 1.0 | core, 128 bits | 82,322 |
| int4 weights on RVV | core | 68,297 |
| IME 0.9, 2x2 tiles | in core | 207,689 |
| AME, 4x4 squares | in core | 70,910 |
| AME | attached tile, near-core | 85,700 |
| DMA-attached accelerator, 16 MACs/cycle | tile port at L2 | 26,240 |
| RVV | 4,096 bits | 43,523 |
| IME / AME | 32x32 | 33,401 / 36,605 |
| DMA accelerator, 1,024 MACs/cycle | | 17,116 |

The 100 MiB agent (51 tokens): fp32 on 32x32 squares 4.72 M cycles per
token at 37.6 MB per token; int4 on the vector unit 11.3 M at 5.6 MB;
both bound by DRAM latency with no prefetcher in the configuration,
which is what the stream engine of the accelerator form would remove.

---

## 6. Verification record

Re-run on 2026-10-06 against commit `1dcf651` of `dev-mimicos-v2-update`
(80-core Debian 12 host; clang 15 with lld for the guests; the RISC-V
Unified Database at `b4e9fea` and the AME draft at `8d27acd` -- commits of
those two specification repositories, not of this tree -- with the IME 0.9
pipeline's UDB overlay, as the specification roots), the same way as
the companion document (`make doc-verify`): every `# verify:` block
extracted in order, the `env` block sourced, each other block in a
traced subshell with its status recorded and its output read.  Every
block passed.

| block | result |
|---|---|
| `env`, `build` | build with the RISC-V and QEMU features; the patched riscv64 QEMU and the Sail model present in the dependency cache |
| `decoder` | tables check passes; regeneration from the specification roots is byte-identical to the committed tables, header and class fragment (1,436 instructions from 68 extensions, 38 classes); the AME overlay regenerates |
| `gemm3` | exit 100; 6,992 instructions, 308 footprints, 41 AME instructions claimed at translation; every class count and footprint byte exact (`expected_counts.py --ame`) |
| `gemm3-32-sail` | the 32x32 build exits 100 on 2.44 M instructions (8,236 footprints); the Sail model reports `SUCCESS` |
| `agent-kernels` | scalar, RVV, IME and int4 exit 100 (276 prefilled, 103 generated tokens); AME and DMA on the plugin models; 4,694,780 AME instructions claimed at translation |
| `agent-vlen` | RVV at 65,536 bits exit 100 |
| `agent-simulate` | 379 tokens simulated: 82,323 cycles and 133,436 instructions per token, 4,122,504 vector multiply-adds |
| `agent-tile` | AME on the tile under `oca_l2` and `nca` (26.0 M instructions, 3.32 M footprints) with `ame_tile.mma` exact; the DMA accelerator with its task count and bytes |
| `agent-32` | IME and AME at 32x32 exit 100 with `ame_n=32` (17.1 M instructions, 71,762 AME instructions claimed); simulated in the core |
| `quantize` | the committed int4 files regenerate identically (40/40) |
| `train` | a 300-step model trained, built and run: every line as numpy (exit 6, some answers wrong, as expected of a short training) |
| `vlen` | the probe agrees at 128, 1,024, 4,096 and 65,536 bits; the gather at 65,536 bits records its footprints and counts the dropped lines |
| `plugin-api` | mode 0 exit 100 with the CSRs, ownership and a claimed product checked; `claim=0` trapped; without the plugin SIGILL; mode 1 SIGILL and mode 2 SIGSEGV with the model's report; 4 M instructions claimed at translation against 4 M through the exception path, timed; the loop probe exits 100 |
| `gdb` | the target description carries `org.riscv.ame` with 103 registers; `amenlen` reads 4, `ameudsz` 32 |
| `patch` | the patch regenerates from the two trees; on this host the riscv64 tree also carries the plugin-wait patch of the Pac-Sim-on-QEMU work, so the regenerated file adds those hunks and is otherwise identical to the committed one |
| `harness` | the listed cases pass |
| `long:wide`, `long:ime-check` | see below |

The earlier record (2026-09-20/21, against `344f0d5` of `dev-mimicos-v2`)
is superseded by this one.

Corrected during the earlier pass (each re-run to a pass): the generator's
report must not be piped into `head` (the pipe closes before the tables
are written); the Sail model's `--config-override` takes a JSON file, not
an inline string; the class count is 38, not 37.  Fixed in the tree:
`gen_isa.py` did not create its `--out` directory (commit `344f0d5`).

`long:wide` generates the 100 MiB model
and its int4 form and reproduces the section 5 rows (fp32 on 32x32
squares 4,720,847 cycles per token at 37.6 MB per token, on the near-core
tile 4,362,537, int4 on the 4,096-bit unit 11,328,192 at 5.6 MB);
`long:ime-check` runs the whole chain, 19 of 19 cases in 221 minutes.
