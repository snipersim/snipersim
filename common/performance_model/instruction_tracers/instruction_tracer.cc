#include "instruction_tracer.h"
#include "simulator.h"
#include "config.hpp"
#include "instruction_tracer_fpstats.h"
#include "instruction_tracer_print.h"
#include "loop_tracer.h"
#include "loop_profiler.h"

void
InstructionTracer::init()
{
   String type = Sim()->getCfg()->getString("instruction_tracer/type");

   if (type == "fpstats")
      InstructionTracerFPStats::init();
}

InstructionTracer*
InstructionTracer::create(const Core *core)
{
   String type = Sim()->getCfg()->getString("instruction_tracer/type");

   if (type == "none")
      return NULL;
   else if (type == "print")
      return new InstructionTracerPrint(core);
   else if (type == "fpstats")
      return new InstructionTracerFPStats(core);
   else if (type == "loop_tracer")
      return new LoopTracer(core);
   else if (type == "loop_profiler")
      return new LoopProfiler(core);
   else
      LOG_PRINT_ERROR("Unknown instruction tracer type %s", type.c_str());
}
