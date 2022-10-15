#ifndef __SYSCALL_MODELING_H
#define __SYSCALL_MODELING_H

#include "pin.H"
#include "sift_writer.h"

// Early versions of Pin and SDE do not include clone3 support

#if !defined(SYS_clone3)
// x86_64 specific
#define SYS_clone3_sniper 435
#endif

#if !defined(clone_args)
// From https://man7.org/linux/man-pages/man2/clone.2.html
struct clone_args_sniper {

    uint64_t flags;        /* Flags bit mask */

    uint64_t pidfd;        /* Where to store PID file descriptor
                              (int *) */

    uint64_t child_tid;    /* Where to store child TID,
                              in child's memory (pid_t *) */

    uint64_t parent_tid;   /* Where to store child TID,
                              in parent's memory (pid_t *) */

    uint64_t exit_signal;  /* Signal to deliver to parent on
                              child termination */

    uint64_t stack;        /* Pointer to lowest byte of stack */

    uint64_t stack_size;   /* Size of stack */

    uint64_t tls;          /* Location of new TLS */

    uint64_t set_tid;      /* Pointer to a pid_t array
                              (since Linux 5.5) */

    uint64_t set_tid_size; /* Number of elements in set_tid
                              (since Linux 5.5) */

    uint64_t cgroup;       /* File descriptor for target cgroup
                              of child (since Linux 5.7) */
};
#endif


VOID emulateSyscallFunc(THREADID threadid, CONTEXT *ctxt);
bool handleAccessMemory(void *arg, Sift::MemoryLockType lock_signal, Sift::MemoryOpType mem_op, uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size);

void initSyscallModeling();

#endif // __SYSCALL_MODELING_H
