#ifndef __HANDLE_SYSCALLS_H__
#define __HANDLE_SYSCALLS_H__

#include "syscall_model.h"
#include "pin.H"

void handleFutexSyscall(CONTEXT *ctx);
void syscallEnterRunModel(THREADID threadIndex, CONTEXT *ctx, SYSCALL_STANDARD syscall_standard, void* v);
void syscallExitRunModel(THREADID threadIndex, CONTEXT *ctx, SYSCALL_STANDARD syscall_standard, void* v);
void contextChange (THREADID threadIndex, CONTEXT_CHANGE_REASON context_change_reason, const CONTEXT *from, CONTEXT *to, INT32 info, VOID *v);
void threadStart (THREADID threadIndex, CONTEXT *ctx, INT32 flags, VOID *v);

SyscallMdl::syscall_args_t syscallArgs (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyRtsigprocmaskContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreRtsigprocmaskContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);

void modifyCloneContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreCloneContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyRtsigsuspendContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreRtsigsuspendContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyRtsigactionContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreRtsigactionContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyNanosleepContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreNanosleepContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyUnameContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreUnameContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifySet_thread_areaContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreSet_thread_areaContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyTimeContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreTimeContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyTimesContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreTimesContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyCloneContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreCloneContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyGettimeofdayContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreGettimofdayContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void modifyGetrlimitContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreGetrlimitContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);

#ifdef TARGET_X86_64
void modifyArch_prctlContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
void restoreArch_prctlContext (CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard);
#endif

#endif /* __HANDLE_SYSCALLS_H__ */
