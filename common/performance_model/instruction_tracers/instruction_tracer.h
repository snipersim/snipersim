#ifndef __INSTRUCTION_TRACER_H
#define __INSTRUCTION_TRACER_H

#include "fixed_types.h"
#include "subsecond_time.h"

class Core;
class DynamicMicroOp;

class InstructionTracer
{
   public:
      static void init();
      static InstructionTracer* create(const Core *core);

      virtual ~InstructionTracer() {}

      typedef struct {
         SubsecondTime dispatch, issue, done, commit;
      } uop_times_t;

      virtual void traceInstruction(const DynamicMicroOp *uop, uop_times_t *times) = 0;
};

#endif // __INSTRUCTION_TRACER_H
