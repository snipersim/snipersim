/* A functional model of the DMA-attached accelerator behind the Xsnacc task
 * hints (tools/riscv/overlay .../Xsnacc): acc.task reads a descriptor and the
 * input operands and computes Y = X W^T + bias into a buffer; acc.wait
 * delivers Y to guest memory.  Every operand block is reported as a
 * footprint (one contiguous range each) so the trace carries what the
 * accelerator's DMA moves.  Shares the Env of the AME model. */
#ifndef ACC_BACKEND_H
#define ACC_BACKEND_H

#include "ame_backend.h"
#include <vector>

namespace acc
{
   /* The descriptor acc.task points at (test/llm-agent/agent.c -DUSE_DMA). */
   struct Descriptor
   {
      uint64_t x, w, bias, y;      /* guest addresses; bias may be 0 */
      uint32_t t, nin, nout, pad;
   };

   struct Backend
   {
      Descriptor desc;
      std::vector<float> y;        /* the result, until acc.wait delivers it */
      bool pending = false;
      uint64_t tasks = 0, errors = 0;
   };

   bool execute(Backend &b, unsigned op, uint32_t word, const ame::Env &env);
}

#endif
