#include "syscall_modeling.h"
#include "sift_assert.h"
#include "globals.h"
#include "threads.h"
#include "timed_calls.h"

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

   const bool live = KnobEmulateSyscalls.Value() && thread_data[threadid].output;

   /* A raw clock_gettime or gettimeofday with no simulator to answer it (a
      trace written to a file): the one-IPC model's time from the
      simulator's time zero, as the libc routines get (emulation.cc), never
      the host's.  time(2) too, which the simulator does not model: its
      time, live.  The call is skipped as an emulated one is. */
   if (!live && !replayingPinball()
       && (syscall_number == SYS_clock_gettime || syscall_number == SYS_gettimeofday || syscall_number == SYS_time))
   {
      const bool tod = syscall_number == SYS_gettimeofday, secs = syscall_number == SYS_time;
      const int clk = tod || secs ? 0 : (int)args[0];
      const bool thread_cpu = clk == CLOCK_THREAD_CPUTIME_ID || (clk < 0 && (clk & 4));
      const bool process_cpu = !thread_cpu && (clk == CLOCK_PROCESS_CPUTIME_ID || clk < 0);
      const uint64_t ns = thread_cpu ? timedThreadCpu(threadid) : process_cpu ? timedProcessCpu()
                        : guestTimeZeroNs() + timedNow(threadid);
      const uint64_t words[2] = { ns / 1000000000ull, tod ? ns / 1000 % 1000000 : ns % 1000000000ull };
      const ADDRINT out = tod || secs ? args[0] : args[1];
      if (out)
         PIN_SafeCopy((void*)out, words, secs ? sizeof(words[0]) : sizeof(words));
      thread_data[threadid].last_syscall_returnval = secs ? words[0] : 0;
      thread_data[threadid].last_syscall_emulated = true;
      return;
   }
   if (live && !replayingPinball() && syscall_number == SYS_time)
   {
      Sift::EmuRequest req;
      Sift::EmuReply res;
      if (thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res))
      {
         const uint64_t s = res.gettime.time_ns / Sift::NS_PER_S;
         if (args[0])
            PIN_SafeCopy((void*)args[0], &s, sizeof(s));
         thread_data[threadid].last_syscall_returnval = s;
         thread_data[threadid].last_syscall_emulated = true;
         return;
      }
   }

   /* Every thread clone, live or not: the child's clear-tid word (for the
      joiner's wake, live), its TLS (which pairs it with the child) and its
      parent (whose time the child's clock starts at).  The simulator hears
      of a new thread on a live channel. */
   if ((syscall_number == SYS_clone && (args[0] & CLONE_THREAD))
       || (syscall_number == SYS_clone3_sniper && args[0] && (((struct clone_args_sniper*)args[0])->flags & CLONE_THREAD)))
   {
      ADDRINT tidptr, tls;
      if (syscall_number == SYS_clone3_sniper)
      {
         struct clone_args_sniper* clone3_args = (struct clone_args_sniper*)args[0];
         /* the word the kernel clears at the child's exit (CLONE_CHILD_CLEARTID) */
         tidptr = (clone3_args->flags & CLONE_CHILD_CLEARTID) ? clone3_args->child_tid : 0;
         tls = (clone3_args->flags & CLONE_SETTLS) ? clone3_args->tls : 0;
      }
      else
      {
      #if defined(TARGET_IA32)
         tidptr = args[2];
         tls = 0;   // i386 passes a user_desc, not a base: oldest-first pairing
      #else
         tidptr = args[3];
         tls = (args[0] & CLONE_SETTLS) ? args[4] : 0;
      #endif
      }
      PIN_GetLock(&new_threadid_lock, threadid);
      tidptrs.push_back({tidptr, tls, (INT32)threadid});
      PIN_ReleaseLock(&new_threadid_lock);
      if (live)
         thread_data[threadid].output->NewThread();   /* New thread */
   }

   /* The timed waits the simulator does not model, in the guest's time
      (timed_calls.cc) */
   {
      ADDRINT r;
      if (timedSyscall(threadid, syscall_number, args, &r))
      {
         thread_data[threadid].last_syscall_returnval = r;
         thread_data[threadid].last_syscall_emulated = true;
         return;
      }
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
            /* the channel went (the simulator ended the application): no
               answer -- the writer's 1 would be returned with the call's
               buffer unwritten -- so the call runs */
            if (!thread_data[threadid].output->IsOpen())
               thread_data[threadid].last_syscall_emulated = false;
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
   timedSyscallReturned(threadid, PIN_GetSyscallReturn(ctxt, syscall_standard));
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
