#include "lite/handle_syscalls.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"
#include "syscall_model.h"
#include "performance_model.h"
#include "log.h"
#include "syscall_strings.h"
#include "local_storage.h"

#include <syscall.h>
#include <stdlib.h>

namespace lite
{

void handleSyscall(THREADID threadIndex, CONTEXT* ctx)
{
   // We shouldn't block on actual SYS_futex calls while inside the SyscallEntry function
   // Therefore, do all the work here, which is at INS_InsertCall(syscall, IPOINT_BEFORE)

   IntPtr syscall_number = PIN_GetContextReg (ctx, REG_GAX);
   LOG_PRINT("Syscall Number(%d)", syscall_number);
   //printf("Entering syscall %s(%ld)\n", syscall_string(syscall_number), syscall_number);


   SyscallMdl::syscall_args_t args;

#ifdef TARGET_IA32
   args.arg0 = PIN_GetContextReg (ctx, REG_GBX);
   args.arg1 = PIN_GetContextReg (ctx, REG_GCX);
   args.arg2 = PIN_GetContextReg (ctx, REG_GDX);
   args.arg3 = PIN_GetContextReg (ctx, REG_GSI);
   args.arg4 = PIN_GetContextReg (ctx, REG_GDI);
   args.arg5 = PIN_GetContextReg (ctx, REG_GBP);
#endif

#ifdef TARGET_INTEL64
   // FIXME: The LEVEL_BASE:: ugliness is required by the fact that REG_R8 etc
   // are also defined in /usr/include/sys/ucontext.h
   args.arg0 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GDI);
   args.arg1 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GSI);
   args.arg2 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_GDX);
   args.arg3 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_R10);
   args.arg4 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_R8);
   args.arg5 = PIN_GetContextReg (ctx, LEVEL_BASE::REG_R9);
#endif

   if (syscall_number == SYS_clone)
   {
      #if defined(TARGET_IA32)
         localStore[threadIndex].pthread_create.tid_ptr = (void*)args.arg2;
      #elif defined(TARGET_INTEL64)
         localStore[threadIndex].pthread_create.tid_ptr = (void*)args.arg3;
      #endif
      localStore[threadIndex].pthread_create.clear_tid = args.arg0 & CLONE_CHILD_CLEARTID ? true : false;
   }

   Thread* thread = Sim()->getThreadManager()->getCurrentThread(threadIndex);
   LOG_ASSERT_ERROR(thread != NULL, "Thread(NULL)");
   thread->getSyscallMdl()->runEnter(syscall_number, args);
}

void syscallEnterRunModel(THREADID threadIndex, CONTEXT* ctx, SYSCALL_STANDARD syscall_standard, void* v)
{
   Thread* thread = Sim()->getThreadManager()->getCurrentThread(threadIndex);
   LOG_ASSERT_ERROR(thread != NULL, "Thread(NULL)");
   if (thread->getSyscallMdl()->isEmulated())
   {
      PIN_SetSyscallNumber(ctx, syscall_standard, SYS_getpid);
   }
}

void syscallExitRunModel(THREADID threadIndex, CONTEXT* ctx, SYSCALL_STANDARD syscall_standard, void* v)
{
   Thread* thread = Sim()->getThreadManager()->getCurrentThread(threadIndex);
   LOG_ASSERT_ERROR(thread != NULL, "Thread(NULL)");
   IntPtr old_return_val = PIN_GetSyscallReturn(ctx, syscall_standard);

   if (thread->getSyscallMdl()->isEmulated())
   {
      IntPtr syscall_return = thread->getSyscallMdl()->runExit(old_return_val);
      PIN_SetContextReg(ctx, REG_GAX, syscall_return);

      LOG_PRINT("Syscall returned (%p)", syscall_return);
   }
   else
   {
      thread->getSyscallMdl()->runExit(old_return_val);
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
