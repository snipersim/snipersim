#include "lite/memory_modeling.h"
#include "simulator.h"
#include "performance_model.h"
#include "core_manager.h"
#include "core.h"
#include "thread.h"
#include "inst_mode.h"
#include "instruction_modeling.h"
#include "dynamic_instruction.h"
#include "fault_injection.h"
#include "inst_mode_macros.h"
#include "local_storage.h"
#include "toolreg.h"

namespace lite
{

Lock g_atomic_lock;

void addMemoryModeling(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode)
{
   if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
   {
      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         if (INS_MemoryOperandIsRead(ins, i))
         {
            if (Sim()->getFaultinjectionManager())
            {
               LOG_ASSERT_ERROR(i < TOOLREG_NUM_MEM, "Insufficient number of TOOLREG_MEMx available");
               LOG_ASSERT_ERROR(i < ThreadLocalStorage::NUM_SCRATCHPADS, "Insufficient number of ThreadLocalStorage::NUM_SCRATCHPADS available");

               INSTRUMENT(
                     INSTR_IF_FASTFORWARD(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryReadFaultinjectionNondetailed),
                     IARG_BOOL, INS_MemoryOperandIsWritten(ins, i) ? INS_IsAtomicUpdate(ins) : false,
                     IARG_MEMORYOP_EA, i,
                     IARG_REG_REFERENCE, g_toolregs[TOOLREG_EA0 + i],
                     IARG_RETURN_REGS, g_toolregs[TOOLREG_MEM0 + i],
                     IARG_END);

               INSTRUMENT(
                     INSTR_IF_CACHEONLY(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryReadFaultinjection),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, ADDRINT(0),
                     IARG_BOOL, INS_MemoryOperandIsWritten(ins, i) ? INS_IsAtomicUpdate(ins) : false,
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_UINT32, i,
                     IARG_REG_REFERENCE, g_toolregs[TOOLREG_EA0 + i],
                     IARG_RETURN_REGS, g_toolregs[TOOLREG_MEM0 + i],
                     IARG_END);

               INSTRUMENT(
                     INSTR_IF_DETAILED(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryReadFaultinjection),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, INS_Address(ins),
                     IARG_BOOL, INS_MemoryOperandIsWritten(ins, i) ? INS_IsAtomicUpdate(ins) : false,
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_UINT32, i,
                     IARG_REG_REFERENCE, g_toolregs[TOOLREG_EA0 + i],
                     IARG_RETURN_REGS, g_toolregs[TOOLREG_MEM0 + i],
                     IARG_END);

               INS_RewriteMemoryOperand(ins, i, g_toolregs[TOOLREG_MEM0 + i]);

               if (INS_MemoryOperandIsWritten(ins, i))
               {
                  INS_InsertPredicatedCall(ins, IPOINT_AFTER, AFUNPTR(lite::completeMemoryWrite),
                     IARG_BOOL, INS_IsAtomicUpdate(ins),
                     IARG_REG_VALUE, g_toolregs[TOOLREG_EA0 + i],
                     IARG_REG_VALUE, g_toolregs[TOOLREG_MEM0 + i],
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_END);
               }
            }
            else
            {
               INSTRUMENT(
                     INSTR_IF_CACHEONLY(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryRead),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, INS_Address(ins),
                     IARG_BOOL, INS_MemoryOperandIsWritten(ins, i) ? INS_IsAtomicUpdate(ins) : false,
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_UINT32, i,
                     IARG_END);

               INSTRUMENT(
                     INSTR_IF_DETAILED(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     Sim()->getConfig()->getIssueMemopsAtFunctional() ? AFUNPTR(lite::handleMemoryReadDetailedIssue) : AFUNPTR(lite::handleMemoryReadDetailed),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, INS_Address(ins),
                     IARG_BOOL, INS_MemoryOperandIsWritten(ins, i) ? INS_IsAtomicUpdate(ins) : false,
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_UINT32, i,
                     IARG_END);
            }
         }
      }

      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         if (INS_MemoryOperandIsWritten(ins, i))
         {
            if (Sim()->getFaultinjectionManager())
            {
               INSTRUMENT(
                     INSTR_IF_CACHEONLY(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryWriteFaultinjection),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, ADDRINT(0),
                     IARG_BOOL, INS_IsAtomicUpdate(ins),
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_END);

               INSTRUMENT(
                     INSTR_IF_DETAILED(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryWriteFaultinjection),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, INS_Address(ins),
                     IARG_BOOL, INS_IsAtomicUpdate(ins),
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_END);
            }
            else
            {
               INSTRUMENT(
                     INSTR_IF_CACHEONLY(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     AFUNPTR(lite::handleMemoryWrite),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, INS_Address(ins),
                     IARG_BOOL, INS_IsAtomicUpdate(ins),
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_UINT32, i,
                     IARG_END);

               INSTRUMENT(
                     INSTR_IF_DETAILED(inst_mode),
                     trace, ins, IPOINT_BEFORE,
                     Sim()->getConfig()->getIssueMemopsAtFunctional() ? AFUNPTR(lite::handleMemoryWriteDetailedIssue) : AFUNPTR(lite::handleMemoryWriteDetailed),
                     IARG_THREAD_ID,
                     IARG_EXECUTING,
                     IARG_ADDRINT, INS_Address(ins),
                     IARG_BOOL, INS_IsAtomicUpdate(ins),
                     IARG_MEMORYOP_EA, i,
                     IARG_UINT32, INS_MemoryOperandSize(ins, i),
                     IARG_UINT32, i,
                     IARG_END);
            }
         }
      }
   }
}

void handleMemoryRead(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size)
{
   Core *core = localStore[thread_id].thread->getCore();
   assert(core);
   if (executing)
      core->accessMemoryFast(false, Core::READ, read_address);
}

void handleMemoryReadDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size)
{
   // Detailed mode: core model will do its own access, just log the address
   assert(localStore[thread_id].dynins);
   localStore[thread_id].dynins->addMemory(executing, SubsecondTime::Zero(), read_address, read_data_size, Operand::READ, 0, HitWhere::UNKNOWN);
}

void handleMemoryReadDetailedIssue(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size)
{
   Core *core = localStore[thread_id].thread->getCore();
   assert(core);
   MemoryResult res;

   if (executing)
   {
      res = core->accessMemory(
            (is_atomic_update) ? Core::LOCK : Core::NONE,
            (is_atomic_update) ? Core::READ_EX : Core::READ,
            read_address,
            NULL,
            read_data_size,
            eip ? Core::MEM_MODELED_RETURN : Core::MEM_MODELED_COUNT,
            eip);
   }
   else
   {
      res = makeMemoryResult(HitWhere::PREDICATE_FALSE, SubsecondTime::Zero());
   }

   if (eip)
   {
      assert(localStore[thread_id].dynins);
      localStore[thread_id].dynins->addMemory(executing, res.latency, read_address, read_data_size, Operand::READ, 0, res.hit_where);
   }
}

ADDRINT handleMemoryReadFaultinjectionNondetailed(bool is_atomic_update, ADDRINT read_address, ADDRINT *save_ea)
{
   *save_ea = read_address;

   if (is_atomic_update)
      g_atomic_lock.acquire();

   return read_address;
}

ADDRINT handleMemoryReadFaultinjection(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, ADDRINT read_address, UInt32 read_data_size, UInt32 op_num, ADDRINT *save_ea)
{
   *save_ea = read_address;

   if (is_atomic_update)
      g_atomic_lock.acquire();

   Core* core = localStore[thread_id].thread->getCore();

   if (executing)
   {
      char buf_fault[1024];
      char *buf_data = localStore[thread_id].scratch[op_num];

      // Load fault mask from simulated memory
      MemoryResult memres = core->accessMemory(
            /*(is_atomic_update) ? Core::LOCK :*/ Core::NONE,
            (is_atomic_update) ? Core::READ_EX : Core::READ,
            read_address,
            buf_fault,
            read_data_size,
            eip ? Core::MEM_MODELED_RETURN : Core::MEM_MODELED_COUNT,
            eip,
            SubsecondTime::MaxTime(),
            true);

      if (eip)
      {
         assert(localStore[thread_id].dynins);
         localStore[thread_id].dynins->addMemory(executing, memres.latency, read_address, read_data_size, Operand::READ, 0, memres.hit_where);
      }

      // Load correct data from real memory
      PIN_SafeCopy(buf_data, (const void*)read_address, read_data_size);

      // Apply the fault mask to the data
      Sim()->getFaultinjectionManager()->applyFault(core, read_address, read_data_size, memres, (Byte*)buf_data, (const Byte*)buf_fault);

      // Address to do the actual read from: read from our data buffer
      return (ADDRINT)buf_data;
   }
   else if (eip)
   {
      // CMOV with false predicate, no actual write, but performance model expects a MemoryInfo
      assert(localStore[thread_id].dynins);
      localStore[thread_id].dynins->addMemory(false, SubsecondTime::Zero(), read_address, read_data_size, Operand::READ, 0, HitWhere::PREDICATE_FALSE);
   }

   return read_address;
}

void completeMemoryWrite(bool is_atomic_update, ADDRINT write_address, ADDRINT scratch, UINT32 write_size)
{
   PIN_SafeCopy((void*)write_address, (void*)scratch, write_size);

   if (is_atomic_update)
      g_atomic_lock.release();
}

void handleMemoryWrite(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size)
{
   /* Optimization for atomic instructions: we know the second (write) access will be a hit. Also, since this is Lite mode,
      we don't need to write back the data. Therefore, only tell the cache to add a hit to its counters and push the
      appropriate DynamicInstructionInfo, but don't keep the cache hierarchy locked for potentially a very long time. */
   Core* core = localStore[thread_id].thread->getCore();
   if (is_atomic_update && executing)
      core->logMemoryHit(false, Core::WRITE, write_address, Core::MEM_MODELED_COUNT, eip);
   else if (executing)
      core->accessMemoryFast(false, Core::WRITE, write_address);
}

void handleMemoryWriteDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size)
{
   /* Optimization for atomic instructions: we know the second (write) access will be a hit. Also, since this is Lite mode,
      we don't need to write back the data. Therefore, only tell the cache to add a hit to its counters and push the
      appropriate DynamicInstructionInfo, but don't keep the cache hierarchy locked for potentially a very long time. */
   Core* core = localStore[thread_id].thread->getCore();
   if (is_atomic_update && executing)
   {
      core->logMemoryHit(false, Core::WRITE, write_address, Core::MEM_MODELED_RETURN, eip);
      if (eip)
      {
         assert(localStore[thread_id].dynins);
         localStore[thread_id].dynins->addMemory(executing, SubsecondTime::Zero(), write_address, write_data_size, Operand::WRITE, 0, HitWhere::L1_OWN);
      }
   }
   else if (eip)
   {
      // Detailed mode: core model will do its own access, just log the address
      assert(localStore[thread_id].dynins);
      localStore[thread_id].dynins->addMemory(executing, SubsecondTime::Zero(), write_address, write_data_size, Operand::WRITE, 0, HitWhere::UNKNOWN);
   }
}

void handleMemoryWriteDetailedIssue(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size)
{
   Core* core = localStore[thread_id].thread->getCore();
   MemoryResult res;

   if (executing)
   {
      res = core->accessMemory(
            (is_atomic_update) ? Core::UNLOCK : Core::NONE,
            Core::WRITE,
            write_address,
            NULL,
            write_data_size,
            eip ? Core::MEM_MODELED_RETURN : Core::MEM_MODELED_COUNT,
            eip);
   }
   else
   {
      res = makeMemoryResult(HitWhere::PREDICATE_FALSE, SubsecondTime::Zero());
   }

   if (eip)
   {
      assert(localStore[thread_id].dynins);
      localStore[thread_id].dynins->addMemory(executing, res.latency, write_address, write_data_size, Operand::WRITE, 0, res.hit_where);
   }
}

char g_zeros[1024] = { 0 };

void handleMemoryWriteFaultinjection(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size)
{
   /* Optimization for atomic instructions: we know the second (write) access will be a hit. Also, since this is Lite mode,
      we don't need to write back the data. Therefore, only tell the cache to add a hit to its counters and push the
      appropriate DynamicInstructionInfo, but don't keep the cache hierarchy locked for potentially a very long time. */
   Core* core = localStore[thread_id].thread->getCore();
   if (is_atomic_update && executing)
   {
      core->accessMemory(
            /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
            Core::WRITE,
            write_address,
            g_zeros,
            write_data_size,
            Core::MEM_MODELED_NONE,
            eip,
            SubsecondTime::MaxTime(),
            false);

      core->logMemoryHit(false, Core::WRITE, write_address, eip ? Core::MEM_MODELED_RETURN : Core::MEM_MODELED_COUNT, eip);
      if (eip)
      {
         assert(localStore[thread_id].dynins);
         localStore[thread_id].dynins->addMemory(executing, SubsecondTime::Zero(), write_address, write_data_size, Operand::WRITE, 0, HitWhere::L1_OWN);
      }
   }
   else if (executing)
   {
      MemoryResult res = core->accessMemory(
            /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
            Core::WRITE,
            write_address,
            g_zeros,
            write_data_size,
            eip ? Core::MEM_MODELED_RETURN : Core::MEM_MODELED_COUNT,
            eip,
            SubsecondTime::MaxTime(),
            false);
      if (eip)
      {
         assert(localStore[thread_id].dynins);
         localStore[thread_id].dynins->addMemory(executing, res.latency, write_address, write_data_size, Operand::WRITE, 0, res.hit_where);
      }
   }
   else if (eip)
   {
      // CMOV with false predicate, no actual write, but performance model expects a MemoryInfo
      assert(localStore[thread_id].dynins);
      localStore[thread_id].dynins->addMemory(false, SubsecondTime::Zero(), write_address, write_data_size, Operand::WRITE, 0, HitWhere::PREDICATE_FALSE);
   }
}

}
