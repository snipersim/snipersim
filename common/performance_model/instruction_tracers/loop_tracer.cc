#include "loop_tracer.h"
#include "simulator.h"
#include "config.hpp"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"

LoopTracer::LoopTracer(const Core *core)
   : m_core(core)
   , m_address_base(strtol(Sim()->getCfg()->getStringArray("loop_tracer/base_address", core->getId()).c_str(), NULL, 16))
   , m_iter_start(Sim()->getCfg()->getIntArray("loop_tracer/iter_start", core->getId()))
   , m_iter_count(Sim()->getCfg()->getIntArray("loop_tracer/iter_count", core->getId()))
   , m_active(false)
   , m_iter_current(-1)
   , m_iter_instr(0)
   , m_instr_uop(0)
   , m_cycle_min(UINT64_MAX)
   , m_cycle_max(0)
   , m_disas_max(0)
   , m_instructions()
{
   LOG_ASSERT_ERROR(m_iter_count <= 36, "Can track only up to 36 iterations");
}

LoopTracer::~LoopTracer()
{
   uint64_t num_cycles = std::min(uint64_t(1000), m_cycle_max - m_cycle_min + 1);   // show at most 1000 cycles
   m_disas_max = std::max(uint64_t(20), m_disas_max);    // make disassembly column at least 20 characters wide

   // print header: major tick every ten cycles, last digit every cycle

   printf("                   %s", String(m_disas_max, ' ').c_str());
   for(uint64_t i = 0; i < num_cycles; i += 10)
      printf("%-5" PRIu64 "               ", i);
   printf("\n");

   printf("                   %s", String(m_disas_max, ' ').c_str());
   for(uint64_t i = 0; i < num_cycles; ++i)
      printf("%" PRIu64 " ", i % 10);
   printf("\n");

   // print per-instruction schedule

   for(Instructions::iterator it = m_instructions.begin(); it != m_instructions.end(); ++it)
   {
      printf("[%8" PRIxPTR "] %-*s (%u)    ", it->second.instruction->getAddress(), int(m_disas_max), it->second.instruction->getDisassembly().c_str(), it->second.uop_num);
      std::vector<char> line(num_cycles, ' ');
      for(std::map<uint64_t, int64_t>::iterator jt = it->second.issued.begin(); jt != it->second.issued.end(); ++jt)
         if (jt->second - m_cycle_min < num_cycles && jt->first < 36)
            line[jt->second - m_cycle_min] = jt->first < 10 ? '0'+jt->first : 'a'+jt->first-10;
      for(std::vector<char>::iterator jt = line.begin(); jt != line.end(); ++jt)
         printf("%c ", *jt);
      printf("\n");
   }
}

void
LoopTracer::traceInstruction(const DynamicMicroOp *uop, uop_times_t *times)
{
   // Ignore dynamic (fake) instructions
   if (!uop->getMicroOp()->getInstruction())
      return;

   uint64_t cycle_issue = 0;
   if (times)
   {
      cycle_issue = SubsecondTime::divideRounded(times->issue, m_core->getDvfsDomain()->getPeriod());
   }

   Instruction *inst = uop->getMicroOp()->getInstruction();
   IntPtr address = inst->getAddress();

   // Start of a new loop iteration?
   if (address == m_address_base && uop->isFirst())
   {
      ++m_iter_current;
      m_iter_instr = 0;
      m_instr_uop = 0;

      if (m_iter_current >= m_iter_start && m_iter_current < m_iter_start + m_iter_count)
         m_active = true;
      else
         m_active = false;

   } else if (address < m_address_base)
      // We have jumped backwards
      m_active = false;


   if (m_active)
   {
      std::pair<IntPtr, UInt8> opid(address, m_instr_uop);
      if (m_instructions.count(opid) == 0)
      {
         m_instructions[opid] = Instr(inst, m_instr_uop);
         m_disas_max = std::max(m_disas_max, (uint64_t)inst->getDisassembly().length());
      }
      m_instructions[opid].issued[m_iter_current - m_iter_start] = cycle_issue;

      m_cycle_min = std::min(m_cycle_min, cycle_issue);
      m_cycle_max = std::max(m_cycle_max, cycle_issue);

      ++m_instr_uop;
      if (uop->getMicroOp()->isLast())
      {
         ++m_iter_instr;
         m_instr_uop = 0;
      }

      // Record only up to 100 instructions
      if (m_iter_instr > 100)
         m_active = false;
   }
}
