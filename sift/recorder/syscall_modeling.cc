#include "syscall_modeling.h"
#include "sift_assert.h"
#include "globals.h"
#include "threads.h"

#include <iostream>
#include <unistd.h>
#include <syscall.h>
#include <time.h>

bool handleAccessMemory(void *arg, Sift::MemoryLockType lock_signal, Sift::MemoryOpType mem_op, uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
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
      return false;
   }

   if (lock_signal == Sift::MemUnlock)
   {
      PIN_ReleaseLock(&access_memory_lock);
   }

   return true;
}

// Emulate all system calls
// Do this as a regular callback (versus syscall enter/exit functions) as those hold the global pin lock
VOID emulateSyscallFunc(THREADID threadid, CONTEXT *ctxt)
{
   // If we haven't set our tid yet, do this now
   if (thread_data[threadid].should_send_threadinfo)
   {
      thread_data[threadid].should_send_threadinfo = false;

      Sift::EmuRequest req;
      Sift::EmuReply res;
      req.setthreadinfo.tid = syscall(SYS_gettid);
      thread_data[threadid].output->Emulate(Sift::EmuTypeSetThreadInfo, req, res);
   }

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
   #elif defined(TARGET_INTEL64) || defined(TARGET_IA32E)
      args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
      args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
      args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
      args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R10);
      args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R8);
      args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R9);
   #else
      #error "Unknown target architecture, require either TARGET_IA32 or TARGET_INTEL64 | TARGET_IA32E"
   #endif

   if (thread_data[threadid].icount_reported > 0)
   {
      thread_data[threadid].output->InstructionCount(thread_data[threadid].icount_reported);
      thread_data[threadid].icount_reported = 0;
   }

   // Default: not emulated, override later when needed
   thread_data[threadid].last_syscall_emulated = false;

   /* A raw clock_gettime or gettimeofday with no simulator to answer it (a
      trace written to a file): the one-IPC model's time from the
      simulator's time zero, as the libc routines get (emulation.cc), never
      the host's.  The call is skipped as an emulated one is. */
   if (!(KnobEmulateSyscalls.Value() && thread_data[threadid].output) && !replayingPinball()
       && (syscall_number == SYS_clock_gettime || syscall_number == SYS_gettimeofday))
   {
      const bool tod = syscall_number == SYS_gettimeofday;
      const int clk = tod ? 0 : (int)args[0];
      const bool cputime = clk == CLOCK_PROCESS_CPUTIME_ID || clk == CLOCK_THREAD_CPUTIME_ID || clk < 0;
      const uint64_t ns = (cputime ? 0 : Sift::TIME_START_DEFAULT * 1000000000ull)
                        + Sift::one_ipc_ns(thread_data[threadid].icount_total);
      const uint64_t words[2] = { ns / 1000000000ull, tod ? ns / 1000 % 1000000 : ns % 1000000000ull };
      const ADDRINT out = tod ? args[0] : args[1];
      if (out)
         PIN_SafeCopy((void*)out, words, sizeof(words));
      thread_data[threadid].last_syscall_returnval = 0;
      thread_data[threadid].last_syscall_emulated = true;
      return;
   }

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
         case SYS_clone3_sniper:
         {
            struct clone_args_sniper* clone3_args = (struct clone_args_sniper*)args[0];
            // The flags are in the argument struct: testing the pointer
            // (`args[0] && CLONE_THREAD`, always true) treated every clone3 --
            // posix_spawn's included -- as a new thread.
            if (clone3_args && (clone3_args->flags & CLONE_THREAD))
            {
               ADDRINT tidptr = clone3_args->parent_tid;
               ADDRINT tls = (clone3_args->flags & CLONE_SETTLS) ? clone3_args->tls : 0;
               PIN_GetLock(&new_threadid_lock, threadid);
               tidptrs.push_back({tidptr, tls});
               PIN_ReleaseLock(&new_threadid_lock);
               /* New thread */
               thread_data[threadid].output->NewThread();
            }
            else
            {
               /* New process */
               // Nothing to do there, handled in fork() -> to check SYS_clone3 is new
            }
            break;
         }
         case SYS_clone:
         {
            if (args[0] & CLONE_THREAD)
            {
               // Store the thread's tid ptr for later use

               #if defined(TARGET_IA32)
                  ADDRINT tidptr = args[2];
                  ADDRINT tls = 0;   // i386 passes a user_desc, not a base: oldest-first pairing
               #elif defined(TARGET_INTEL64) || defined(TARGET_IA32E)
                  ADDRINT tidptr = args[3];
                  ADDRINT tls = (args[0] & CLONE_SETTLS) ? args[4] : 0;
               #else
                  #error "Unknown target architecture, require either TARGET_IA32 or TARGET_INTEL64 | TARGET_IA32E"
               #endif

               PIN_GetLock(&new_threadid_lock, threadid);
               tidptrs.push_back({tidptr, tls});
               PIN_ReleaseLock(&new_threadid_lock);
               /* New thread */
               thread_data[threadid].output->NewThread();
            }
            else
            {
               /* New process */
               // Nothing to do there, handled in fork()
            }
            break;
         }

         // System calls not emulated (passed through to OS)
         case SYS_read:
         case SYS_write:
         case SYS_wait4:
            thread_data[threadid].last_syscall_number = syscall_number;
            thread_data[threadid].last_syscall_emulated = false;
            thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls emulated (not passed through to OS)
         case SYS_futex:
         case SYS_sched_yield:
         case SYS_sched_setaffinity:
         case SYS_sched_getaffinity:
         case SYS_nanosleep:
         // usleep(3), sleep(3) and nanosleep(3) all reach the kernel as
         // clock_nanosleep since glibc 2.31; left native, the thread sleeps in
         // host time while the simulator believes it is running, and its core
         // never arrives at the barrier.
         case SYS_clock_nanosleep:
         // The clocks, read with a system call rather than through the libc
         // routines the recorder replaces (emulation.cc): the host's time
         // otherwise
         case SYS_clock_gettime:
         case SYS_gettimeofday:
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
