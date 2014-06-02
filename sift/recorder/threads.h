#ifndef __THREAD_INFO_H
#define __THREAD_INFO_H

#include "globals.h"
#include "sift_writer.h"
#include "bbv_count.h"

#include "pin.H"
#include <deque>

typedef struct {
   Sift::Writer *output;
   UINT64 dyn_addresses[Sift::MAX_DYNAMIC_ADDRESSES];
   UINT32 num_dyn_addresses;
   Bbv *bbv;
   UINT64 thread_num;
   ADDRINT bbv_base;
   UINT64 bbv_count;
   ADDRINT bbv_last;
   BOOL bbv_end;
   UINT64 blocknum;
   UINT64 icount;
   UINT64 icount_cacheonly;
   UINT64 icount_cacheonly_pending;
   UINT64 icount_detailed;
   UINT64 icount_reported;
   ADDRINT last_syscall_number;
   ADDRINT last_syscall_returnval;
   UINT64 flowcontrol_target;
   ADDRINT tid_ptr;
   ADDRINT last_routine;
   ADDRINT last_call_site;
   BOOL last_syscall_emulated;
   BOOL running;
   BOOL should_send_threadinfo;
} __attribute__((packed,aligned(LINE_SIZE_BYTES))) thread_data_t;

extern thread_data_t *thread_data;

void initThreads();

#endif // __THREAD_INFO_H
