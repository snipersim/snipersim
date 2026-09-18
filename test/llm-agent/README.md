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
outputs).  The matrix variants keep the RVV dot product for attention and
for the odd output rows (the 45-entry head).  All four must produce the
tokens the numpy model produces.  At batch 1 a matrix unit computes a
full tile for one useful column, which is the batch-1 cost to
measure; the AME build runs only under the frontend plugin
(QEMU has no AME) and dies with SIGILL without it.

Harness cases (`testing/test_qemu.py`): `riscv-llm-agent` runs the four
kernels under the IME QEMU (AME through the plugin) and requires the same
answers from each; `riscv-llm-simulate` simulates the RVV, IME and AME
builds in Sniper, reports cycles and instructions per token for each, and
requires the matrix-unit instruction counts to equal what the model's
shape implies (per forward, per layer D->3D, D->D, D->4D, 4D->D, then
D->V: tile or square loads, multiply-accumulates, stores, configuration).

Build by hand:

    clang --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding \
          -nostdlib -static -fuse-ld=lld -Wl,-e,_start [-DUSE_RVV|-DUSE_IME|-DUSE_AME] \
          -I test/llm-agent -I include -o agent test/llm-agent/agent.c
    qemu-riscv/build/qemu-riscv64 -cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on ./agent
    # the AME build needs the plugin:
    qemu-riscv/build/qemu-riscv64 -cpu max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on \
          -plugin build/release/lib/libsniper-qemu.so,output=trace -- ./agent
