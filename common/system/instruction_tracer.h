#ifndef __INSTRUCTION_TRACER_H
#define __INSTRUCTION_TRACER_H

class Instruction;

class InstructionTracer
{
   public:
      static void init();
      static InstructionTracer* create(int core_id);

      InstructionTracer() {}
      virtual ~InstructionTracer() {}

      virtual void handleInstruction(Instruction const* instruction) = 0;
};

#endif // __INSTRUCTION_TRACER_H
