#pragma once

#include "fixed_types.h"
#include "pin.H"

namespace lite
{

void handleSyscall(THREADID threadIndex, CONTEXT* ctx);
void syscallEnterRunModel(THREADID threadIndex, CONTEXT* ctx, SYSCALL_STANDARD syscall_standard, void* v);
void syscallExitRunModel(THREADID threadIndex, CONTEXT* ctx, SYSCALL_STANDARD syscall_standard, void* v);
bool interceptSignal(THREADID threadIndex, INT32 signal, CONTEXT *ctx, BOOL hasHandler, const EXCEPTION_INFO *pExceptInfo, void* v);

}
