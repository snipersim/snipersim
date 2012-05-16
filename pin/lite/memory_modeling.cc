#include "lite/memory_modeling.h"
#include "simulator.h"
#include "performance_model.h"
#include "core_manager.h"
#include "core.h"
#include "thread.h"
#include "pin_memory_manager.h"
#include "inst_mode.h"
#include "instruction_modeling.h"
#include "inst_mode_macros.h"
#include "local_storage.h"
#include "toolreg.h"

namespace lite
{

Lock g_atomic_lock;

void addMemoryModeling(TRACE trace, INS ins)
{
   if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
   {
      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         if (INS_MemoryOperandIsRead(ins, i))
         {
            INSTRUMENT(
                  INSTR_IF_CACHEONLY,
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
                  INSTR_IF_DETAILED,
                  trace, ins, IPOINT_BEFORE,
                  AFUNPTR(lite::handleMemoryReadDetailed),
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

      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         if (INS_MemoryOperandIsWritten(ins, i))
         {
            INSTRUMENT(
                  INSTR_IF_CACHEONLY,
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
                  INSTR_IF_DETAILED,
                  trace, ins, IPOINT_BEFORE,
                  AFUNPTR(lite::handleMemoryWriteDetailed),
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

void handleMemoryRead(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(thread_id);
   if (executing)
      core->accessMemory(
            /*(is_atomic_update) ? Core::LOCK :*/ Core::NONE,
            (is_atomic_update) ? Core::READ_EX : Core::READ,
            read_address,
            NULL,
            read_data_size,
            Core::MEM_MODELED_COUNT,
            0);
}

void handleMemoryReadDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(thread_id);
   // Detailed mode: core model will do its own access, just push a dyninfo with the address
   DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(eip, executing, SubsecondTime::Zero(), read_address, read_data_size, Operand::READ, 0, HitWhere::UNKNOWN);
   core->getPerformanceModel()->pushDynamicInstructionInfo(info);
}

void handleMemoryWrite(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size)
{
   /* Optimization for atomic instructions: we know the second (write) access will be a hit. Also, since this is Lite mode,
      we don't need to write back the data. Therefore, only tell the cache to add a hit to its counters and push the
      appropriate DynamicInstructionInfo, but don't keep the cache hierarchy locked for potentially a very long time. */
   Core* core = localStore[thread_id].thread->getCore();
   if (is_atomic_update && executing)
      core->logMemoryHit(false, Core::WRITE, write_address, localStore[thread_id].inst_mode == InstMode::DETAILED ? Core::MEM_MODELED_DYNINFO : Core::MEM_MODELED_COUNT, eip);
   else if (executing)
      core->accessMemory(
            /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
            Core::WRITE,
            write_address,
            NULL,
            write_data_size,
            Core::MEM_MODELED_COUNT,
            0);
}

void handleMemoryWriteDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size)
{
   /* Optimization for atomic instructions: we know the second (write) access will be a hit. Also, since this is Lite mode,
      we don't need to write back the data. Therefore, only tell the cache to add a hit to its counters and push the
      appropriate DynamicInstructionInfo, but don't keep the cache hierarchy locked for potentially a very long time. */
   Core* core = localStore[thread_id].thread->getCore();
   if (is_atomic_update && executing)
      core->logMemoryHit(false, Core::WRITE, write_address, localStore[thread_id].inst_mode == InstMode::DETAILED ? Core::MEM_MODELED_DYNINFO : Core::MEM_MODELED_COUNT, eip);
   else {
      // Detailed mode: core model will do its own access, just push a dyninfo with the address
      DynamicInstructionInfo info = DynamicInstructionInfo::createMemoryInfo(eip, executing, SubsecondTime::Zero(), write_address, write_data_size, Operand::WRITE, 0, HitWhere::UNKNOWN);
      core->getPerformanceModel()->pushDynamicInstructionInfo(info);
   }
}

}
