#include "lite/handle_syscalls.h"
#include "simulator.h"
#include "core_manager.h"
#include "core.h"
#include "syscall_model.h"
#include "performance_model.h"
#include "log.h"

#include <syscall.h>
#include <stdlib.h>

namespace lite
{

void handleFutexSyscall(THREADID threadIndex, CONTEXT* ctx)
{
   ADDRINT syscall_number = PIN_GetContextReg (ctx, REG_GAX);
   if (syscall_number != SYS_futex)
      return;

   SyscallMdl::syscall_args_t args;

#ifdef TARGET_IA32
   args.arg0 = PIN_GetContextReg (ctx, REG_GBX);
   args.arg1 = PIN_GetContextReg (ctx, REG_GCX);
   args.arg2 = PIN_GetContextReg (ctx, REG_GDX);
   args.arg3 = PIN_GetContextReg (ctx, REG_GSI);
   args.arg4 = PIN_GetContextReg (ctx, REG_GDI);
   args.arg5 = PIN_GetContextReg (ctx, REG_GBP);
#endif

#ifdef TARGET_X86_64
   // FIXME: The LEVEL_BASE:: ugliness is required by the fact that REG_R8 etc
   // are also defined in /usr/include/sys/ucontext.h
   args.arg0 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GDI);
   args.arg1 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GSI);
   args.arg2 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GDX);
   args.arg3 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_R10);
   args.arg4 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_R8);
   args.arg5 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_R9);
#endif

   Core* core = Sim()->getCoreManager()->getCurrentCore(threadIndex);

   LOG_ASSERT_ERROR(core != NULL, "Core(NULL)");
   LOG_PRINT("syscall_number %d", syscall_number);

   core->getSyscallMdl()->runEnter(syscall_number, args);
}

void syscallEnterRunModel(THREADID threadIndex, CONTEXT* ctx, SYSCALL_STANDARD syscall_standard, void* v)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(threadIndex);
   LOG_ASSERT_ERROR(core, "Core(NULL)");

   IntPtr syscall_number = PIN_GetSyscallNumber(ctx, syscall_standard);
   LOG_PRINT("Syscall Number(%d)", syscall_number);

   // Save the syscall number
   core->getSyscallMdl()->saveSyscallNumber(syscall_number);
   if (syscall_number == SYS_futex
      || syscall_number == SYS_clock_gettime)
   {
      PIN_SetSyscallNumber(ctx, syscall_standard, SYS_getpid);
   }
}

void syscallExitRunModel(THREADID threadIndex, CONTEXT* ctx, SYSCALL_STANDARD syscall_standard, void* v)
{
   Core* core = Sim()->getCoreManager()->getCurrentCore(threadIndex);
   LOG_ASSERT_ERROR(core, "Core(NULL)");

   IntPtr syscall_number = core->getSyscallMdl()->retrieveSyscallNumber();
   if (syscall_number == SYS_futex)
   {
      IntPtr old_return_val = PIN_GetSyscallReturn (ctx, syscall_standard);
      IntPtr syscall_return = core->getSyscallMdl()->runExit(old_return_val);
      PIN_SetContextReg(ctx, REG_GAX, syscall_return);

      LOG_PRINT("Syscall(%p) returned (%p)", syscall_number, syscall_return);

   } else if (syscall_number == SYS_clock_gettime) {
      clockid_t clock = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GDI);
      struct timespec *ts = (struct timespec *)PIN_GetContextReg (ctx, LEVEL_BASE::REG_GSI);
      SubsecondTime time = core->getPerformanceModel()->getElapsedTime();
      IntPtr syscall_return;

      switch(clock) {
         case CLOCK_REALTIME:
         case CLOCK_MONOTONIC:
            ts->tv_sec = time.getNS() / 1000000000;
            ts->tv_nsec = time.getNS() % 1000000000;
            syscall_return = 0;
            break;
         default:
            LOG_ASSERT_ERROR(false, "SYS_clock_gettime does not currently support clock(%u)", clock);
      }
   }
}

bool interceptSignal(THREADID threadIndex, INT32 signal, CONTEXT *ctx, BOOL hasHandler, const EXCEPTION_INFO *pExceptInfo, void* v)
{
   ADDRINT eip = PIN_GetContextReg(ctx, REG_INST_PTR);
   fprintf(stderr, "Application received fatal signal %u at eip %p\n", signal, (void*)eip);

   INT32 column, line;
   string fileName;
   PIN_GetSourceLocation(eip, &column, &line, &fileName);
   fprintf(stderr, "in %s:%u\n", fileName.c_str(), line);

   switch(PIN_GetDebugStatus())
   {
      case DEBUG_STATUS_CONNECTED:
         // A debugger is already connected, it should just break
         break;
      // TODO: This is supposed to work in a future Pin version. Right now, a fatal signal will kill Pin & the application
      case DEBUG_STATUS_UNCONNECTED:
         // The -appdebug_enable switch was used, but no debugger is currently connected
         // Instruct the user to do this now
         DEBUG_CONNECTION_INFO info;
         if (!PIN_GetDebugConnectionInfo(&info) || info._type != DEBUG_CONNECTION_TYPE_TCP_SERVER)
            break;
         fprintf(stderr, "\nStart GDB and enter this command:\n  target remote :%d\n\n", info._tcpServer._tcpPort);
         PIN_WaitForDebuggerToConnect(0);
         break;
      case DEBUG_STATUS_UNCONNECTABLE:
         // Application debugging is enabled, but it is too early to allow a debugger to connect
         fprintf(stderr, "\nApplication debugging is enabled, but it is too early to allow a debugger to connect.\n\n");
         exit(0);
      case DEBUG_STATUS_DISABLED:
         // No -appdebug* switch given to Pin, nothing we can do
         fprintf(stderr, "\nApplication debugging is currently disabled.\nTo enable it, start Sniper with the --appdebug or --appdebug-wait command line switch.\n\n");
         exit(0);
   }

   return true;
}

}
