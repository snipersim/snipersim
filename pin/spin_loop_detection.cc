// Pin front-end for the spin loop detector implemented by the SpinLoopDetector class

#include "spin_loop_detection.h"
#include "inst_mode_macros.h"
#include "local_storage.h"

#include "simulator.h"
#include "thread.h"

#include <algorithm>

static ADDRINT spinloopIfInSpin(THREADID thread_id)
{
   // Condition used for If/Then instrumentation, should be inlineable
   return localStore[thread_id].sld.sld->inCandidateSpin();
}

static void spinloopHandleBranch(THREADID thread_id, ADDRINT eip, BOOL taken, ADDRINT target)
{
   if (taken && target < eip)
   {
      localStore[thread_id].sld.sld->commitBCT(eip);
   }
}

static void spinloopHandleWriteBefore(THREADID thread_id, ADDRINT addr)
{
   // Remember store address and old value
   localStore[thread_id].sld.write_addr = addr;
   localStore[thread_id].sld.write_value = *(uint64_t *)addr;
}

static void spinloopHandleWriteAfter(THREADID thread_id)
{
   intptr_t addr = localStore[thread_id].sld.write_addr;
   uint64_t old_value = localStore[thread_id].sld.write_value;
   bool silent = *(uint64_t *)addr == old_value;

   if (silent == false)
   {
      localStore[thread_id].sld.sld->commitNonSilentStore();
   }
}

static void spinloopHandleRegWriteBefore(THREADID thread_id, UINT32 reg, ADDRINT value)
{
   // Remember old value for register that is about to be written
   localStore[thread_id].sld.reg_value[reg] = value;
}

static void spinloopHandleRegWriteAfter(THREADID thread_id, UINT32 _reg, ADDRINT value)
{
   REG reg = (REG)_reg;
   ADDRINT old_value = localStore[thread_id].sld.reg_value[reg];

   localStore[thread_id].sld.sld->commitRegisterWrite(reg, old_value, value);
}

void addSpinLoopDetection(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode)
{
   if (INS_HasFallThrough(ins))
   {
      // Always look for new BCTs

      if (INS_IsBranch(ins))
      {
         INSTRUMENT_PREDICATED(
            INSTR_IF_DETAILED(inst_mode),
            trace, ins, IPOINT_TAKEN_BRANCH, (AFUNPTR)spinloopHandleBranch,
            IARG_THREAD_ID,
            IARG_ADDRINT, INS_Address(ins),
            IARG_BOOL, TRUE,
            IARG_BRANCH_TARGET_ADDR,
            IARG_END);

         INSTRUMENT_PREDICATED(
            INSTR_IF_DETAILED(inst_mode),
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
               INSTR_IF_DETAILED(inst_mode),
               trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
               IARG_THREAD_ID,
               IARG_END);
         INSTRUMENT_THEN_PREDICATED(
               INSTR_IF_DETAILED(inst_mode),
               trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopHandleWriteBefore,
               IARG_THREAD_ID,
               IARG_MEMORYWRITE_EA,
               IARG_END);
         INSTRUMENT_IF_PREDICATED(
               INSTR_IF_DETAILED(inst_mode),
               trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
               IARG_THREAD_ID,
               IARG_END);
         INSTRUMENT_THEN_PREDICATED(
               INSTR_IF_DETAILED(inst_mode),
               trace, ins, IPOINT_AFTER, (AFUNPTR)spinloopHandleWriteAfter,
               IARG_THREAD_ID,
               IARG_END);
      }

      for (unsigned int i = 0; i < INS_MaxNumWRegs(ins); i++)
      {
         REG reg = INS_RegW(ins, i);
         // Limit ourselves to those registers for which IARG_REG_VALUE is valid
         // This excludes YMM registers, but hopefully a loop that updates YMM registers also updates an iteration counter
         // in a general purpose register so we'll still recognize the loop as updating architecture state (and hence not a spin)
         if (reg < REG_MACHINE_LAST && (REG_is_gr(reg) || REG_is_gr8(reg) || REG_is_gr16(reg) || REG_is_gr32(reg) || REG_is_gr64(reg)))
         {
            INSTRUMENT_IF_PREDICATED(
                  INSTR_IF_DETAILED(inst_mode),
                  trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
                  IARG_THREAD_ID,
                  IARG_END);
            INSTRUMENT_THEN_PREDICATED(
                  INSTR_IF_DETAILED(inst_mode),
                  trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopHandleRegWriteBefore,
                  IARG_THREAD_ID,
                  IARG_UINT32, (uint32_t)reg,
                  IARG_REG_VALUE, reg,
                  IARG_END);
            INSTRUMENT_IF_PREDICATED(
                  INSTR_IF_DETAILED(inst_mode),
                  trace, ins, IPOINT_BEFORE, (AFUNPTR)spinloopIfInSpin,
                  IARG_THREAD_ID,
                  IARG_END);
            INSTRUMENT_THEN_PREDICATED(
                  INSTR_IF_DETAILED(inst_mode),
                  trace, ins, IPOINT_AFTER, (AFUNPTR)spinloopHandleRegWriteAfter,
                  IARG_THREAD_ID,
                  IARG_UINT32, (uint32_t)reg,
                  IARG_REG_VALUE, reg,
                  IARG_END);
         }
      }
   }
}
