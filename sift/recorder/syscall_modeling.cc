#include "syscall_modeling.h"
#include "sift_assert.h"
#include "globals.h"
#include "threads.h"

#include <iostream>
#include <syscall.h>

void handleAccessMemory(void *arg, Sift::MemoryLockType lock_signal, Sift::MemoryOpType mem_op, uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
{
   // Lock memory globally if requested
   // This operation does not occur very frequently, so this should not impact performance
   if (lock_signal == Sift::MemLock)
   {
      PIN_GetLock(&access_memory_lock, 0);
   }

   if (mem_op == Sift::MemRead)
   {
      // The simulator is requesting data from us
      PIN_SafeCopy(data_buffer, reinterpret_cast<void*>(d_addr), data_size);
   }
   else if (mem_op == Sift::MemWrite)
   {
      // The simulator is requesting that we write data back to memory
      PIN_SafeCopy(reinterpret_cast<void*>(d_addr), data_buffer, data_size);
   }
   else
   {
      std::cerr << "Error: invalid memory operation type" << std::endl;
      sift_assert(false);
   }

   if (lock_signal == Sift::MemUnlock)
   {
      PIN_ReleaseLock(&access_memory_lock);
   }
}

// Emulate all system calls
// Do this as a regular callback (versus syscall enter/exit functions) as those hold the global pin lock
VOID emulateSyscallFunc(THREADID threadid, CONTEXT *ctxt)
{
   ADDRINT syscall_number = PIN_GetContextReg(ctxt, REG_GAX);

   sift_assert(syscall_number < MAX_NUM_SYSCALLS);

   syscall_args_t args;
   #if defined(TARGET_IA32)
      args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GBX);
      args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GCX);
      args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
      args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
      args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
      args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GBP);
   #elif defined(TARGET_INTEL64)
      args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
      args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
      args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
      args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R10);
      args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R8);
      args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R9);
   #else
      #error "Unknown target architecture, require either TARGET_IA32 or TARGET_INTEL64"
   #endif

   // Default: not emulated, override later when needed
   thread_data[threadid].last_syscall_emulated = false;

   if (syscall_number == SYS_write && thread_data[threadid].output)
   {
      int fd = (int)args[0];
      const char *buf = (const char*)args[1];
      size_t count = (size_t)args[2];

      if (count > 0 && (fd == 1 || fd == 2))
         thread_data[threadid].output->Output(fd, buf, count);
   }

   if (KnobEmulateSyscalls.Value() && thread_data[threadid].output)
   {
      switch(syscall_number)
      {
         // Handle SYS_clone child tid capture for proper pthread_join emulation.
         // When the CLONE_CHILD_CLEARTID option is enabled, remember its child_tidptr and
         // then when the thread ends, write 0 to the tid mutex and futex_wake it
         case SYS_clone:
         {
            thread_data[threadid].output->NewThread();
            // Store the thread's tid ptr for later use
            #if defined(TARGET_IA32)
               ADDRINT tidptr = args[2];
            #elif defined(TARGET_INTEL64)
               ADDRINT tidptr = args[3];
            #endif
            PIN_GetLock(&new_threadid_lock, threadid);
            tidptrs.push_back(tidptr);
            PIN_ReleaseLock(&new_threadid_lock);
            break;
         }

         // System calls not emulated (passed through to OS)
         case SYS_write:
            thread_data[threadid].last_syscall_number = syscall_number;
            thread_data[threadid].last_syscall_emulated = false;
            thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls emulated (not passed through to OS)
         case SYS_futex:
         case SYS_sched_yield:
         case SYS_sched_setaffinity:
         case SYS_sched_getaffinity:
            thread_data[threadid].last_syscall_number = syscall_number;
            thread_data[threadid].last_syscall_emulated = true;
            thread_data[threadid].last_syscall_returnval = thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls sent to Sniper, but also passed through to OS
         case SYS_exit_group:
            thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;
      }
   }
}

static VOID syscallEntryCallback(THREADID threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, VOID *v)
{
   if (!thread_data[threadid].last_syscall_emulated)
   {
      return;
   }

   PIN_SetSyscallNumber(ctxt, syscall_standard, SYS_getpid);
}

static VOID syscallExitCallback(THREADID threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, VOID *v)
{
   if (!thread_data[threadid].last_syscall_emulated)
   {
      return;
   }

   PIN_SetContextReg(ctxt, REG_GAX, thread_data[threadid].last_syscall_returnval);
   thread_data[threadid].last_syscall_emulated = false;
}

void initSyscallModeling()
{
   PIN_AddSyscallEntryFunction(syscallEntryCallback, 0);
   PIN_AddSyscallExitFunction(syscallExitCallback, 0);
}
