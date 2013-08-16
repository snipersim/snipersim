#ifndef __THREAD_INFO_H
#define __THREAD_INFO_H

#include "globals.h"
#include "sift_writer.h"
#include "bbv_count.h"

#include "pin.H"
#include <deque>

typedef struct {
   Sift::Writer *output;
   std::deque<ADDRINT> *dyn_address_queue;
   Bbv *bbv;
   UINT64 thread_num;
   ADDRINT bbv_base;
   UINT64 bbv_count;
   ADDRINT bbv_last;
   BOOL bbv_end;
   UINT64 blocknum;
   UINT64 icount;
   UINT64 icount_detailed;
   ADDRINT last_syscall_number;
   ADDRINT last_syscall_returnval;
   UINT64 flowcontrol_target;
   ADDRINT tid_ptr;
   ADDRINT last_routine;
   BOOL last_syscall_emulated;
   BOOL running;
   #if defined(TARGET_IA32)
      uint8_t __pad[41];
   #elif defined(TARGET_INTEL64)
      uint8_t __pad[5];
   #endif
} __attribute__((packed)) thread_data_t;

extern thread_data_t *thread_data;

void initThreads();

#endif // __THREAD_INFO_H
