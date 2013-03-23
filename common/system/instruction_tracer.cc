#include "instruction_tracer.h"
#include "simulator.h"
#include "config.hpp"
#include "instruction_tracer_print.h"
#include "instruction_tracer_fpstats.h"

void InstructionTracer::init()
{
   String type = Sim()->getCfg()->getString("instruction_tracer/type");

   if (type == "none")
      ; /* nop */
   else if (type == "print")
      InstructionTracerPrint::init();
   else if (type == "fpstats")
      InstructionTracerFPStats::init();
   else
      LOG_PRINT_ERROR("Unknown instruction tracer type %s", type.c_str());
}

InstructionTracer* InstructionTracer::create(int core_id)
{
   String type = Sim()->getCfg()->getString("instruction_tracer/type");

   if (type == "none")
      return NULL;
   else if (type == "print")
      return new InstructionTracerPrint(core_id);
   else if (type == "fpstats")
      return new InstructionTracerFPStats(core_id);
   else
      LOG_PRINT_ERROR("Unknown instruction tracer type %s", type.c_str());
}
