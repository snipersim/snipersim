# RISC-V in Sniper, generated from the specification

Sniper's RISC-V support is built from the machine-readable specification,
the [RISC-V Unified Database](https://github.com/riscv/riscv-unified-db)
(UDB), rather than from a hand-maintained decoder.  Drop a new specification
in, regenerate, rebuild: the decoder knows the new instructions, the core
model costs them by class, the QEMU frontend records their memory footprints,
and the statistics count them.  This document is the map of that pipeline
and its next steps.

## The pipeline, end to end

```
 specification (UDB YAML)                       ime-0.9-pipeline (outside Sniper)
   riscv-unified-db  spec/std/isa/inst/**       ├── apply-llvm.mjs   -> LLVM (assembler, codegen)
   + IME 0.9 overlay (public/udb, same layout)  ├── apply-qemu.mjs   -> QEMU (decode, helpers)
   + tools/riscv/overlay (Sniper's additions)   └── UDB YAML  ─────┐
                                                                    v
 tools/riscv/gen_isa.py  +  tools/riscv/isa_policy.yaml   (this repository)
   ├── decoder_lib/riscv_isa_gen.{h,cc}   instruction table, dispatch, class table, self-test data
   ├── include/riscv_insn.h               inline-asm (.insn) macros for extensions a stock assembler lacks
   ├── config/riscv*.cfg                  [perf_model/core/riscv] block: decoder parameters, latencies
   └── (report)                           class membership, overlaps
        v
 decoder_lib/riscv_isa.{h,cc}    API over the tables (decode, operands, disassemble, self-test)
   ├── decoder_lib/riscv_decoder.{h,cc}     Sniper's dl::Decoder for RISC-V
   ├── core_model_boom_v1 / dynamic_micro_op_boom_v1   latency, issue interval, port, unit by class
   ├── common/trace_frontend/trace_thread   riscv.<class> counters, footprint replay
   └── frontend/qemu-plugin                 branch flag and footprint flag from the same tables
```

The workload path is unchanged: the guest is compiled by the pipeline's LLVM,
run under the pipeline's QEMU with Sniper's TCG plugin, which writes a SIFT
trace (or streams it), and Sniper simulates it.  What the generator adds is
that every stage past QEMU learns the ISA from the same YAML QEMU was
patched from.

## What the UDB gives, what the policy adds

From each instruction's YAML the generator takes the name, the defining
extension(s), the `encoding.match` string (mask and match bits, 16 or 32),
the `variables` (operand fields with bit segments, left shift, sign
extension; 3-bit register fields of compressed encodings map to x8-x15) and
the `assembly` template (used for disassembly and to tell loads from stores:
`vd, (xs1)` versus `vs3, (xs1)`).  RV32-only instructions are dropped by
evaluating `definedBy` for xlen 64.  Later `--udb` roots override earlier
ones by instruction name, which is how an extension under review sits on top
of the ratified ISA.

`tools/riscv/isa_policy.yaml` adds what the UDB does not carry:

- which extensions to include (regexes; privileged ones are in for
  system-mode traces),
- an ordered list of class rules -- `ext`, `name` and `asm` regexes, first
  match wins -- giving each instruction a functional-unit class with flags
  (`load`, `store`, `branch`, `jump`, `indirect`, `atomic`, `fp`, `vector`,
  `footprint`, `busy`, `matrix`, `serializing`, `prefetch`, `cache_flush`)
  and a CPI-stack group,
- one placeholder latency per class, and an issue interval where the unit is
  pipelined (the matrix unit),
- the accumulating forms (destination also read), scalar access sizes, and
  the decoder parameters.

Timing is deliberately a placeholder per class: refine a class's latency in
the policy (defaults) or in the config (`perf_model/core/riscv/<class>_latency`,
`<class>_issue_cycles`) without touching code.  `make riscv-isa-gen` (with
`SNIPER_UDB_ROOTS` set at configure time) or a direct run of the generator
rewrites the tables and the config blocks; `testing/test_riscv_isa.py`
fails when the checked-in tables are out of step with the sources.

## AME: a UDB overlay generated from the specification source

The Attached Matrix Extension (AME, extension name Ztt) is the second
matrix extension the path carries, and the first whose working group has
not published UDB YAML: its public repository
(`riscv/riscv-attached-matrix-extension`) is AsciiDoc.  Its instruction
chapter is machine-readable all the same -- every instruction carries a
wavedrom field list (the exact encoding), a mnemonic line, a synopsis, a
description and an operation block, and the repository ships a generated
mask/match table -- so `tools/riscv/gen_ame_udb.py` converts it into the
UDB layout (`tools/riscv/overlay/spec/std/isa/inst/Ztt/*.yaml`,
`ext/Ztt.yaml`), checking every encoding against the repository's own
table (138 instructions, zero mismatches at commit 8d27acd).  Operand
names follow the UDB direction convention (`md`/`ms1`/`ms2` for M
registers, `ad`/`as1` for accumulators, `xd`/`xs1`/`xs2` for X
registers); the spec's pseudocode is carried verbatim as `operation()`
for reference.  From there AME takes exactly IME's path: the policy's
`ame_*` classes (ownership, datatype configuration, load, store, matrix
multiply, move, elementwise) with placeholder latencies, two new register
kinds in the generated tables (M registers 96-127, accumulators 128-143),
and `.insn` macros -- AME's R2/R1 forms put fixed selectors in register
slots, which the macros encode as the register name carrying that value
(`x24` for 0x18) -- verified by assembling them with stock clang and
decoding the words back through the tables.  `SNIPER_AME_ROOT` (the
driver, the CMake target and `testing/test_riscv_isa.py`) regenerates the
overlay from a checkout before the tables, so a moved draft is caught.
When the group publishes AME in UDB form, that root replaces the
generated overlay with no other change.

The functional side takes a different route from IME's: QEMU is not
patched.  The frontend plugin recognises the `ame_*` classes from the
same tables and executes them itself on a model of the unit
(`frontend/qemu-plugin/ame_backend.{h,cc}`) through the plugin API's
register and memory access and `qemu_plugin_set_pc`, so a stock
plugin-enabled qemu-riscv64 runs AME binaries and records their
footprints (`frontend/qemu-plugin/README.md`).  Operand lookup in the
model is by the overlay's operand names (`md`, `ms1`, `as1`, `xd`, ...),
which is why the converter's direction rule matters: the first operand
of a mnemonic is the destination unless the specification names it a
source, so `mmov.m.a md, as1` reads an accumulator and `agettyp xd, as1`
writes an X register.

## Using a new extension with an unmodified compiler

LLVM has no plugin mechanism for new instructions (its plugins are
optimization passes; the MC layer and the TableGen instruction definitions
are compiled in), and a full LLVM rebuild is the wrong price for trying an
extension.  The generator therefore also writes `include/riscv_insn.h`
(`--header`, extensions selected by `header:` in the policy): one macro
per instruction as a typed `.insn` directive, which binutils and LLVM 15+
assemble without knowing the instruction.  Integer register operands are C
expressions the compiler allocates, vector and FP register operands are
register numbers, every other field (vm, lambda, immediates) is an integer
literal folded into the funct7 or immediate expression.  A stock distro
clang with `-march=rv64gcv` (the vector configuration instructions are
standard) then builds an IME workload, and the encodings are the
specification's -- checked byte for byte against the pipeline's llvm-mc.
`test/qemu-plugin-probe/rv-ime.c` is written this way.

When real code generation or intrinsics are wanted, the ime-0.9-pipeline's
LLVM overlay (`generated/llvm-ime`) is the route, and only a RISC-V-only
clang+lld build is needed; the header remains the way to reach encodings
the compiler cannot yet emit itself.

## QEMU with the extension

The frontend needs a QEMU that both implements the extension and loads
plugins -- which rules out static binaries, since they cannot `dlopen`.
`deps/sniper-deps --riscv-ime` (or `make RISCV_IME=ON`) builds QEMU
11.1.1 riscv64 user mode from the pinned source with
`tools/riscv/qemu-11.1.1-ime.patch` (the pipeline's generated QEMU overlay
for freeze 0.9, applied to the v11.1.1 tag) and `--enable-plugins`, once
per version into the shared dependency cache, and links it at
`qemu-riscv/`.  Run it with `-cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on`.
Building on the target is what keeps it free of glibc-version trouble: a
binary built on a newer distribution than the one it runs on fails on
glibc symbol versions, and a static one cannot host the plugin.

## The whole chain in one command

`make riscv-ime-check` runs `tools/riscv/ime-check.sh`: regenerate and
verify the decoder tables (when `SNIPER_UDB_ROOTS` names the checkouts),
build Sniper, build the patched QEMU through the dependency fetcher, find
a clang with a RISC-V target (`SNIPER_RISCV_CC`, or on PATH), and run the
`riscv-*` harness cases, which trace and simulate the IME workload and
check exact instruction, footprint, span and class counts.

## Validation: functional, and what reaches the memory hierarchy

`test/riscv-gemm/gemm3.c` is the workload that spans both spaces: one
int32 matrix product computed by a scalar reference, by RVV 1.0 and by IME
0.9 in one self-verifying binary (exit 100 when all agree, 5/6/7 naming
the disagreement).  Three harness cases hang off it:

- **Functional, under QEMU** (`riscv-gemm-verify`): the three paths agree.
- **Functional, against the specification's model** (`riscv-gemm-sail`):
  the scalar-versus-RVV comparison runs on the prebuilt sail-riscv 0.14
  model, the golden RVV 1.0 reference, at VLEN=128 (a bare-metal HTIF
  variant of the same source; `deps/sniper-deps --riscv-ime` fetches the
  model).  IME is not in the Sail model; its semantics are checked against
  the scalar reference under QEMU, whose helpers derive from the
  specification's Sail.
- **Counts, from the specification** (`riscv-gemm-simulate`):
  `tools/riscv/expected_counts.py` derives, from the UDB's tile geometry
  (`M = (VLEN/SEW)/lambda` rows of `lambda*LMUL` elements) and the
  workload's loop structure, how many instructions of each class and how
  many bytes of vector and tile footprint the run must contain, and
  requires Sniper's `riscv.<class>` counters and `riscv.span_bytes` to
  equal them.  For 8x8x8: 64 `vmmacc.vv`, 144 tile loads, 16 tile stores,
  128 vector loads, 6656 footprint bytes -- all exact.  The frontend
  records footprints as contiguous byte runs (each tile row its own run),
  so the bytes are the bytes moved; two runs in one cache line still cost
  one cache access, since the core squashes the second uop.

## Correctness checks

- `rvisa::self_test()` (run when the decoder is created, and by the test):
  every table row's sample word decodes to the row the generator expected,
  which pins the priority among overlapping encodings (c.nop before c.addi,
  c.ebreak before c.jalr, prefetch.i before ori).
- The QEMU harness cases (`testing/test_qemu.py`, `riscv-*`) run real
  guests through QEMU and Sniper and check exact instruction, branch,
  footprint and class counts.
- When the generated decoder replaced rv8, every distinct instruction word
  of the RISC-V traces in hand was decoded by both: the only differences were
  rv8 reporting compressed instructions under their 32-bit names (c.li as
  addi) and rv8 calling the vector and matrix instructions illegal.

## Dropping in a specification

1. Put the UDB YAML somewhere: a riscv-unified-db checkout for the base
   ISA, and any overlay in the same `spec/std/isa/inst/<Ext>/*.yaml` layout
   for the extension under study (the ime-0.9-pipeline ships one under
   `public/udb`; Sniper's own additions live in `tools/riscv/overlay`).
2. Add the extension's name pattern to `extensions:` in the policy, and a
   class rule for its instructions if the existing ones do not fit (the
   report, `gen_isa.py --report`, lists every class's members so a
   misfiled instruction is visible).
3. Regenerate: `python3 tools/riscv/gen_isa.py --udb <upstream> --udb <overlay>
   --udb tools/riscv/overlay --policy tools/riscv/isa_policy.yaml --out decoder_lib
   --config config/riscv.cfg --config config/riscv-mediumboom.cfg
   --header include/riscv_insn.h`, rebuild, run `testing/test_riscv_isa.py`
   and the `riscv-*` harness cases (`make riscv-ime-check` does all of it).
4. If the instructions touch memory in a shape two addresses cannot
   describe, give the class the `footprint` flag: the frontend then records
   every byte range (see frontend/qemu-plugin/README.md) and the core model
   replays them.

## Execution speed: where the JIT-like paths are, and what comes next

Both sides already execute nothing twice that they can execute once:

- **QEMU** translates each basic block once into TCG; the plugin's
  instrumentation (branch flag, footprint flag, callbacks) is decided at
  translation time per instruction and only the dynamic facts (taken,
  addresses) are reported per execution.
- **Sniper** decodes each static instruction once (`TraceThread::decode`
  caches `DecodedInst` per PC; `InstructionDecoder` caches the micro-op
  list per PC) and the generated decoder's dispatch is a bucket lookup over
  a table, not a search.

What is left on the table, in order of payoff, for a faster path from QEMU
into the core model:

1. **Block-level trace records.**  QEMU knows the static contents of every
   translated block; emitting a block's instructions once (a static block
   record) and then one small record per execution (block id, branch
   outcome, memory addresses) would cut the trace volume and Sniper's
   per-instruction reader work by the average block length.  The SIFT
   `MemorySpans` record already shows the shape of a pre-instruction
   side record; a `BlockDefine`/`BlockExec` pair is the same idea one level
   up.
2. **Shared-memory ring instead of a FIFO.**  The recorder and Sniper
   exchange bytes through pipes; a lock-free ring with the same record
   format removes the copies and the FIFO open-order hazards documented in
   test/pincrt-fifo.
3. **Pre-decoded blocks on Sniper's side.**  With block records the
   `DecodedInst` and micro-op caches become per-block arrays filled once at
   block definition, so the simulation loop runs over prepared micro-ops
   with no per-instruction lookups at all -- the simulator's equivalent of
   a translation cache.
4. **rv8's JIT** (the checkout Sniper's old decoder was built on) is a
   RISC-V-to-x86 binary translator; if a native, QEMU-free fast path is ever
   wanted, its translated blocks could feed the same block records, but
   QEMU with the plugin is the path the specification pipeline patches, so
   it stays the primary frontend.

None of these change the decoder or the policy: the generated tables are
the contract between the frontend's translation-time decisions and the
core model, whichever way the dynamic stream arrives.
