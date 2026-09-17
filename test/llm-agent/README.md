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
dot-product kernel is selectable at build time: scalar, or RVV 1.0
(`-DUSE_RVV`: `vfmacc.vv` accumulation over e32,m4 groups, one
`vfredusum.vs`); both must produce identical tokens.

Harness cases (`testing/test_qemu.py`): `riscv-llm-agent` runs both
kernels under the IME QEMU; `riscv-llm-simulate` simulates the RVV build
in Sniper and reports cycles and instructions per token, the batch-of-1
baseline.

Build by hand:

    clang --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding \
          -nostdlib -static -fuse-ld=lld -Wl,-e,_start [-DUSE_RVV] -I test/llm-agent \
          -o agent test/llm-agent/agent.c
    qemu-riscv/build/qemu-riscv64 -cpu max,vlen=128,x-zvvmm=on,x-zvvmtls=on ./agent
