# llm-agent: a tool-calling GPT on the RISC-V infrastructure

The first end-to-end LLM workload: a nanoGPT-shaped character-level
transformer (d=64, 3 layers, 4 heads, context 64, ~160k parameters) that
answers questions by calling tools:

    Q: what is 12 plus 7?      the prompt
    T: add 12 7                the model writes the tool call
    R: 19                      the runtime executes it and writes the result
    A: 19                      the model writes the answer

Training happens on the host, not here: `tools/llm/train_agent_gpt.py`
(numpy only, manual backprop, gradient-checked) trains on a synthetic
corpus of add/mul/len questions and writes `model_weights.h` (float32,
matmul weights transposed so every output is one contiguous dot product)
and `prompts.h` (questions with the numpy model's own greedy outputs and
the true answers).  Only inference runs on the edge.

`agent.c` is that inference: freestanding (no libc, own exp/tanh, write(2)
for output), a KV-cached decoder with greedy sampling, the agent loop, and
the tools.  It checks every generated line against the numpy model's
output and every answer against the tool, exiting 100 when all agree.  The
kernels are selectable at build time: scalar; RVV 1.0 (`-DUSE_RVV`:
`vfmacc.vv` accumulation over e32,m4 groups, one `vfredusum.vs`); the
weight matvecs on IME 0.9 (`-DUSE_IME`: fp32 2x2 tiles, `vmtl.v` of two
weight rows and of the activation vector over a zero row, `vfmmacc.vv`,
column 0 of the tile is two outputs); or on the AME draft (`-DUSE_AME`:
fp32 4x4 squares, `mls.st` of four weight rows and of the vector over
three zero rows, `mmulbtacc.2d`, `mmov.m.a`, `mss.st`, column 0 is four
outputs); or as tasks for the DMA-attached accelerator (`-DUSE_DMA`: a
descriptor per weight product, `acc.task`, `acc.wait`).  The matrix
variants keep the RVV dot product for attention and for the odd output
rows (the 45-entry head).  All five must produce the tokens the numpy
model produces.  Decode is batch 1, where a matrix unit computes a full
tile for one useful column; prefill runs the queued prompt text as one
batch (`forward_batch`), so every weight matvec is a GEMM over the token
block and the tiles fill, with a weight tile loaded once per group of
token tiles (16 accumulators on AME: the whole 64-token window).  The
AME and DMA builds run only under the frontend plugin (QEMU has neither)
and die with SIGILL without it.  The guest prints the prefill batch
sizes, from which the harness derives the exact tile, square and task
counts.  The unit geometry is a compile-time choice: `-DAME_N=32` for
32x32 squares (the plugin takes `ame_n=32`), `-DIME_VLEN=4096
-DIME_LMUL=8 -DIME_LAMBDA=4` for 32x32 IME tiles (QEMU with
`vlen=4096`); batches shorter than a tile are padded.
ACCELERATOR_COMPONENTS.md section 8 has the limits and the measurements.

Harness cases (`testing/test_qemu.py`): `riscv-llm-agent` runs the five
kernels under the IME QEMU (AME and DMA through the plugin) and requires
the same answers from each; `riscv-llm-simulate` simulates the RVV, IME
and AME builds with the units costed in the core, reports cycles and
instructions per token, and requires the matrix-unit instruction counts
to equal what the model's shape and the prefill batches imply;
`riscv-llm-couplings` simulates the AME build on the attached tile and
the DMA build on the accelerator under the L2 and near-core couplings.
Results are in ACCELERATOR_COMPONENTS.md, section 7.

Build by hand:

    clang --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding \
          -nostdlib -static -fuse-ld=lld -Wl,-e,_start [-DUSE_RVV|-DUSE_IME|-DUSE_AME|-DUSE_DMA] \
          -I test/llm-agent -I include -o agent test/llm-agent/agent.c
    qemu-riscv/build/qemu-riscv64 -cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on ./agent
    # the AME build needs the plugin:
    qemu-riscv/build/qemu-riscv64 -cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on \
          -plugin build/release/lib/libsniper-qemu.so,output=trace -- ./agent

The trainer's `--blob` writes the weights as `model_weights.bin`, a raw
float32 file the agent embeds at link time with `.incbin` (the header then
only carries the shape and the offsets); a 100 MiB model is not C text.
The blob is found through the include path, and because a quoted
`#include` searches the source file's own directory first, a model kept in
another directory is built from a copy of `agent.c` placed outside
`test/llm-agent`.

An untrained model can be exported for measurements of size alone:
`--steps 0 --export-anyway`, with `--eval-questions`, `--prompts` and
`--gen` keeping the reference generation short.  Its outputs are
garbage, but they are the numpy model's garbage: the header pins the
generation lengths (`GEN_CALL`, `GEN_ANS`) to the value the references
used, and the agent feeds the protocol newline after a call the model
never terminated, so the token-for-token check still holds.  Exit code 6
(every line as numpy, every answer wrong) is such a model's pass; the
sweep scripts accept it.

`-DUSE_W4` runs the layer matvecs on int4 weights (`tools/llm/quantize_w4.py`
writes `model_w4.h`, `model_w4.bin` and `prompts_w4.h`: a per-row scale,
planar nibbles, 8x fewer weight bytes than fp32, the int4 numpy model's own
outputs as the reference); the dot product dequantizes on the vector unit.
The harness runs it as the `w4` variant of `riscv-llm-agent` and
`riscv-llm-simulate`.
