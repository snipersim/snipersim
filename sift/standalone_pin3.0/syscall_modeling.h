#ifndef __SYSCALL_MODELING_H
#define __SYSCALL_MODELING_H

#include "pin.H"
#include "sift_writer.h"

VOID emulateSyscallFunc(THREADID threadid, CONTEXT *ctxt);
void handleAccessMemory(void *arg, Sift::MemoryLockType lock_signal, Sift::MemoryOpType mem_op, uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size);

void initSyscallModeling();

#endif // __SYSCALL_MODELING_H
