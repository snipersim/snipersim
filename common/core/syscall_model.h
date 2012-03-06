#ifndef SYSCALL_MODEL_H
#define SYSCALL_MODEL_H

#include "message_types.h"
#include "packetize.h"
#include "transport.h"
#include "network.h"
#include "fixed_types.h"

#include <iostream>

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

      SyscallMdl(Network *net);
      ~SyscallMdl();

      IntPtr runExit(IntPtr old_return);
      IntPtr runEnter(IntPtr syscall_number, syscall_args_t &args);

      // ------------------------------------------------------
      // New stuff added with Memory redirection

      void saveSyscallArgs (syscall_args_t &args);
      void retrieveSyscallArgs (syscall_args_t &args);
      void saveSyscallNumber (IntPtr syscall_number);
      IntPtr retrieveSyscallNumber();

      void *copyArgToBuffer(UInt32 arg_num, IntPtr arg_addr, UInt32 size);
      void copyArgFromBuffer(UInt32 arg_num, IntPtr arg_addr, UInt32 size);

      //---------------------------------------------------------

   private:

      // ------------------------------------------------------
      // New stuff added with Memory redirection

      static const UInt32 m_scratchpad_size = 512;
      static const UInt32 m_num_syscall_args = 6;
     
      static const char *futex_names[16];

      struct futex_counters_t
      {
         uint64_t count[16];
         SubsecondTime delay[16];
      } *futex_counters;

      IntPtr m_syscall_number;
      bool m_called_enter;
      IntPtr m_ret_val;
      struct syscall_args_t m_saved_args;
      char m_scratchpad [m_num_syscall_args] [m_scratchpad_size];

      // ------------------------------------------------------

      UnstructuredBuffer m_send_buff;
      UnstructuredBuffer m_recv_buff;
      Network *m_network;

      IntPtr marshallOpenCall(syscall_args_t &args);
      IntPtr marshallReadCall(syscall_args_t &args);
      IntPtr marshallWriteCall(syscall_args_t &args);
      IntPtr marshallWritevCall(syscall_args_t &args);
      IntPtr marshallCloseCall(syscall_args_t &args);
      IntPtr marshallLseekCall(syscall_args_t &args);
      IntPtr marshallGetcwdCall(syscall_args_t &args);
      IntPtr marshallAccessCall(syscall_args_t &args);
#ifdef TARGET_X86_64
      IntPtr marshallStatCall(syscall_args_t &args);
      IntPtr marshallFstatCall(syscall_args_t &args);
#endif
#ifdef TARGET_IA32
      IntPtr marshallFstat64Call(syscall_args_t &args);
#endif
      IntPtr marshallIoctlCall(syscall_args_t &args);
      IntPtr marshallGetpidCall(syscall_args_t &args);
      IntPtr marshallReadaheadCall(syscall_args_t &args);
      IntPtr marshallPipeCall(syscall_args_t &args);
      IntPtr marshallMmapCall(syscall_args_t &args);
#ifdef TARGET_IA32
      IntPtr marshallMmap2Call(syscall_args_t &args);
#endif
      IntPtr marshallMunmapCall(syscall_args_t &args);
      IntPtr marshallBrkCall(syscall_args_t &args);
      IntPtr marshallFutexCall(syscall_args_t &args);

      // Helper functions
      UInt32 getStrLen (char *str);
      void futexCount(uint32_t function, Core *core, SubsecondTime delay);

};

#endif
