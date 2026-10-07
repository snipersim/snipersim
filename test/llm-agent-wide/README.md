# llm-agent-wide: the 100 MiB agent

The same `test/llm-agent/agent.c`, on a model whose weights no edge chip
holds on chip: d=512, 8 layers, 4 heads, context 128, 25.3 million
parameters (101 MB of fp32).  It is untrained, since training 25 million
parameters in numpy takes days and the measurement needs only the shape:
the prompt references are the untrained numpy model's own outputs, so the
self-check still holds (`GEN_CALL`/`GEN_ANS` in the header pin the C
agent's generation lengths to the numpy value, since an untrained model
never emits the stop character, and the guest exits 6: every line as the
numpy model, the answers wrong).  The generated files are not committed;
regenerate them (about a minute each):

    python3 tools/llm/train_agent_gpt.py --out test/llm-agent-wide --d 512 --layers 8 --ctx 128 \
            --steps 0 --export-anyway --eval-questions 2 --prompts 1 --gen 8 --blob
    python3 tools/llm/quantize_w4.py --dir test/llm-agent-wide --questions 1 --prompts 1 --export-anyway

`--blob` writes the weights as `model_weights.bin`, which `agent.c`
embeds with `.incbin` (a header of 100 MB of C text is not an option);
the quantizer writes `model_w4.bin` (int4, 12.7 MB).

`tools/llm/wide-sweep.sh <out dir> [bandwidth GB/s] [config ...]` builds,
records and simulates every kernel on every unit under an edge memory
bandwidth (25.6 GB/s by default; `config/riscv.cfg` ships 5) and prints
cycles, instructions and DRAM accesses per token:

| config           | kernel and unit                                                     |
|------------------|---------------------------------------------------------------------|
| `fp32-rvv`       | fp32 weights, RVV dot products, 128-bit vector unit                  |
| `fp32-rvv4k`     | the same on a 4,096-bit vector unit over a 64-byte datapath          |
| `fp32-ame32`     | fp32 weights on 32x32 AME squares, in the core                       |
| `fp32-ame32-nca` | the same on the attached tile with its own port (`coupling = nca`)  |
| `w4-rvv`         | int4 weights dequantized on the 128-bit vector unit                  |
| `w4-rvv4k`       | the same on the 4,096-bit vector unit                                |

The harness case `riscv-llm-wide` runs `fp32-ame32` and `w4-rvv4k`, the
two fast ones, when the model is present (a few hundred million
instructions each; `fp32-rvv` records 877 million and simulates for
hours).  Results: docs/ACCELERATOR_COMPONENTS.md section 9.
