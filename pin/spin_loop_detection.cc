// Implements spin loop detection as described by
// T. Li, A. R. Lebeck, and D. J. Sorin. "Spin detection hardware
// for improved management of multithreaded systems."
// IEEE Transactions on Parallel and Distributed Systems (TPDS), 17:508-521, June 2006.

#include "spin_loop_detection.h"
#include "inst_mode_macros.h"
#include "local_storage.h"

#include "simulator.h"
#include "thread.h"

#include <algorithm>

static ADDRINT spinloopIfInSpin(THREADID thread_id)
{
   // Condition used for If/Then instrumentation, should be inlineable
   return !localStore[thread_id].sld->sdt.empty();
}

static void spinloopHandleBranch(THREADID thread_id, ADDRINT eip, BOOL taken, ADDRINT target)
{
   if (taken && target < eip)
   {
      // We just executed a backward control transfer (BCT)
      // ``A BCT can be a backward conditional taken branch, unconditional branch, or jump instruction.''

      // Note: since we keep the SDT as a queue, a BCT's position is not constant so we use explicit ids
      // We also have an unbounded RUB so there is no overflow bit

      // Find BCT in the SDT
      std::deque<std::pair<ADDRINT, uint8_t> >::iterator entry = localStore[thread_id].sld->sdt.begin();
      for( ; entry != localStore[thread_id].sld->sdt.end(); ++entry)
         if (entry->first == eip)
            break;

      if (entry == localStore[thread_id].sld->sdt.end())
      {
         // Unknown BCT: add to SDT, increment next SDT entry id, and update mask of valid ids
         localStore[thread_id].sld->sdt.push_back(std::make_pair(eip, localStore[thread_id].sld->sdt_nextid));
         localStore[thread_id].sld->sdt_nextid = (localStore[thread_id].sld->sdt_nextid + 1) % SDT_MAX_SIZE;

         // Keep only top SDT_MAX_SIZE entries
         if (localStore[thread_id].sld->sdt.size() > SDT_MAX_SIZE)
            localStore[thread_id].sld->sdt.pop_front();

         localStore[thread_id].sld->sdt_bitmask = 0;
         for(std::deque<std::pair<ADDRINT, uint8_t> >::iterator it = localStore[thread_id].sld->sdt.begin(); it != localStore[thread_id].sld->sdt.end(); ++it)
            localStore[thread_id].sld->sdt_bitmask |= (1 << it->second);
      }
      else
      {
         // Found! Check all RUB entries for our id to check if observable register state
         // has changed since the end of last iteration (Spin Condition 1)
         uint8_t id = entry->second;

         bool spinning = true;
         for(std::unordered_map<REG, std::pair<ADDRINT, uint16_t> >::iterator it = localStore[thread_id].sld->rub.begin(); it != localStore[thread_id].sld->rub.end(); ++it)
         {
            if (it->second.second & (1 << id))
            {
               // ``There exists at least one entry in the RUB that corresponds to this SDT entry.
               //   This means that the observable register state of the thread differs and, thus,
               //   the thread was not spinning.''
               spinning = false;
               break;
            }
         }

         if (spinning)
         {
            // Spin loop detected !!
            // TODO: do something interesting now...
         }

         // Next iteration should use a new id (RUB entries correspond to our previous iteration)
         entry->second = localStore[thread_id].sld->sdt_nextid;
         localStore[thread_id].sld->sdt_nextid = (localStore[thread_id].sld->sdt_nextid + 1) % SDT_MAX_SIZE;
      }
   }
}

static void spinloopHandleWriteBefore(THREADID thread_id, ADDRINT addr)
{
   // Remember store address and old value
   localStore[thread_id].sld->write_addr = addr;
   localStore[thread_id].sld->write_value = *(uint64_t *)addr;
}

static void spinloopHandleWriteAfter(THREADID thread_id)
{
   intptr_t addr = localStore[thread_id].sld->write_addr;
   uint64_t old_value = localStore[thread_id].sld->write_value;
   bool silent = *(uint64_t *)addr == old_value;

   if (silent == false)
   {
      // ``Whenever the processor commits a non-silent store, it clears the SDT.''
      localStore[thread_id].sld->sdt.clear();
      localStore[thread_id].sld->rub.clear();
   }
}

static void spinloopHandleRegWriteBefore(THREADID thread_id, UINT32 reg, ADDRINT value)
{
   // Remember old value for register that is about to be written
   localStore[thread_id].sld->reg_value[reg] = value;
}

static void spinloopHandleRegWriteAfter(THREADID thread_id, UINT32 _reg, ADDRINT value)
{
   // Implements Register Update Buffer (RUB), see Section 3.2, 3rd paragraph
   REG reg = (REG)_reg;

   // ``For each instruction it then commits, the processor checks if the instruction's architectural
   //   (i.e., logical) destination register is already in the RUB.''
   if (localStore[thread_id].sld->rub.count(reg) == 0)
   {
      // ``If not, the processor has discovered a new register written by the thread
      //   It then compares the new value of this register (i.e., the value being committed)
      //   to its old value (i.e., the value before being overwritten by the commit).''
      ADDRINT old_value = localStore[thread_id].sld->reg_value[reg];
      if (value != old_value)
      {
         // ``If not equal, the processor adds the register number and its old value to the RUB.''
         localStore[thread_id].sld->rub[reg] = std::make_pair(old_value, localStore[thread_id].sld->sdt_bitmask);
      }
   }
   else
   {
      // ``If the register is already in the RUB, then the processor compares its new value to its current value in the RUB.''
      if (value == localStore[thread_id].sld->rub[reg].first)
      {
         // ``If equal, it deletes this register from the RUB;''
         localStore[thread_id].sld->rub.erase(reg);
      }
      // ``otherwise, no action is necessary.''
   }
}

void addSpinLoopDetection(TRACE trace, INS ins)
{
   if (INS_HasFallThrough(ins))
   {
      // Always look for new BCTs

      if (INS_IsBranch(ins))
      {
         INSTRUMENT_PREDICATED(
            INSTR_IF_DETAILED,
            trace, ins, IPOINT_TAKEN_BRANCH, (AFUNPTR)spinloopHandleBranch,
            IARG_THREAD_ID,
            IARG_ADDRINT, INS_Address(ins),
            IARG_BOOL, TRUE,
            IARG_BRANCH_TARGET_ADDR,
            IARG_END);

         INSTRUMENT_PREDICATED(
            INSTR_IF_DETAILED,
            trace, ins, IPOINT_AFTER, (AFUNPTR)spinloopHandleBranch,
            IARG_THREAD_ID,
            IARG_ADDRINT, INS_Address(ins),
            IARG_BOOL, FALSE,
            IARG_BRANCH_TARGET_ADDR,
            IARG_END);
      }

      // The below (non-silent store detection, and RUB updates), are only needed
      // when the SDT is non-empty (i.e., we may be in a spin). Use If/Then instrumentation
      // to only execute these calls when needed.

      if (INS_IsMemoryWrite(ins))
      {
         INSTRUMENT_IF_PREDICATED(
               INSTR_IF_DETAILED,
               trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
               IARG_THREAD_ID,
               IARG_END);
         INSTRUMENT_THEN_PREDICATED(
               INSTR_IF_DETAILED,
               trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopHandleWriteBefore,
               IARG_THREAD_ID,
               IARG_MEMORYWRITE_EA,
               IARG_END);
         INSTRUMENT_IF_PREDICATED(
               INSTR_IF_DETAILED,
               trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
               IARG_THREAD_ID,
               IARG_END);
         INSTRUMENT_THEN_PREDICATED(
               INSTR_IF_DETAILED,
               trace, ins, IPOINT_AFTER, (AFUNPTR)spinloopHandleWriteAfter,
               IARG_THREAD_ID,
               IARG_END);
      }

      for (unsigned int i = 0; i < INS_MaxNumWRegs(ins); i++)
      {
         REG reg = INS_RegW(ins, i);
         if (reg < REG_MACHINE_LAST && (REG_is_gr(reg) || REG_is_gr8(reg) || REG_is_gr16(reg) || REG_is_gr32(reg) || REG_is_gr64(reg)))
         {
            INSTRUMENT_IF_PREDICATED(
                  INSTR_IF_DETAILED,
                  trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
                  IARG_THREAD_ID,
                  IARG_END);
            INSTRUMENT_THEN_PREDICATED(
                  INSTR_IF_DETAILED,
                  trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopHandleRegWriteBefore,
                  IARG_THREAD_ID,
                  IARG_UINT32, (uint32_t)reg,
                  IARG_REG_VALUE, reg,
                  IARG_END);
            INSTRUMENT_IF_PREDICATED(
                  INSTR_IF_DETAILED,
                  trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
                  IARG_THREAD_ID,
                  IARG_END);
            INSTRUMENT_THEN_PREDICATED(
                  INSTR_IF_DETAILED,
                  trace, ins, IPOINT_AFTER, (AFUNPTR)spinloopHandleRegWriteAfter,
                  IARG_THREAD_ID,
                  IARG_UINT32, (uint32_t)reg,
                  IARG_REG_VALUE, reg,
                  IARG_END);
         }
      }
   }
}
