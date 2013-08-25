#ifndef SYSCALL_MODEL_H
#define SYSCALL_MODEL_H

#include "fixed_types.h"
#include "subsecond_time.h"

#include <iostream>

class Thread;

class SyscallMdl
{
   public:
      struct syscall_args_t
      {
          IntPtr arg0;
          IntPtr arg1;
          IntPtr arg2;
          IntPtr arg3;
          IntPtr arg4;
          IntPtr arg5;
      };

      struct HookSyscallEnter
      {
         thread_id_t thread_id;
         core_id_t core_id;
         SubsecondTime time;
         IntPtr syscall_number;
         syscall_args_t args;
      };

      struct HookSyscallExit
      {
         thread_id_t thread_id;
         core_id_t core_id;
         SubsecondTime time;
         IntPtr ret_val;
         bool emulated;
      };

      SyscallMdl(Thread *thread);
      ~SyscallMdl();

      void runEnter(IntPtr syscall_number, syscall_args_t &args);
      IntPtr runExit(IntPtr old_return);
      bool isEmulated() const { return m_emulated; }
      bool inSyscall() const { return m_in_syscall; }
      String formatSyscall() const;

   private:
      static const char *futex_names[];

      struct futex_counters_t
      {
         uint64_t count[16];
         SubsecondTime delay[16];
      } *futex_counters;

      Thread *m_thread;
      IntPtr m_syscall_number;
      bool m_emulated;
      IntPtr m_ret_val;
      bool m_in_syscall;
      syscall_args_t m_syscall_args;

      // ------------------------------------------------------

      IntPtr handleFutexCall(syscall_args_t &args);

      // Helper functions
      void futexCount(uint32_t function, SubsecondTime delay);
};

#endif
