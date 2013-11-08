#ifndef __LOOP_TRACER_H
#define __LOOP_TRACER_H

#include "instruction_tracer.h"

#include <map>

class Core;
class Instruction;
class MicroOp;
class DynamicMicroOp;

class LoopTracer : public InstructionTracer
{
   private:
      const Core *m_core;

      const IntPtr m_address_base;
      const SInt64 m_iter_start;
      const SInt64 m_iter_count;

      bool m_active;
      SInt64 m_iter_current;
      UInt64 m_iter_instr;
      UInt8 m_instr_uop;
      uint64_t m_cycle_min;
      uint64_t m_cycle_max;
      uint64_t m_disas_max;

      class Instr {
         public:
            Instr() {}
            Instr(const Instruction *instruction, UInt8 uop_num) : instruction(instruction), uop_num(uop_num), issued() {}
            const Instruction *instruction;
            UInt8 uop_num;
            std::map<uint64_t, int64_t> issued;
      };
      typedef std::map<std::pair<IntPtr, UInt8>, Instr> Instructions;
      Instructions m_instructions;

   public:
      LoopTracer(const Core *core);
      virtual ~LoopTracer();

      virtual void traceInstruction(const DynamicMicroOp *uop, uop_times_t *times);
};

#endif // __LOOP_TRACER_H
