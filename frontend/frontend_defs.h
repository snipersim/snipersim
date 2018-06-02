#ifndef __FRONTEND_DEFS_H
#define __FRONTEND_DEFS_H

#include "sift_format.h"
#include "sift_writer.h"
#include "bbv_count.h"

#define LINE_SIZE_BYTES 64
#define MAX_NUM_THREADS 128
#define MAX_NUM_SYSCALLS 4096

// Type declarations
#if defined(TARGET_IA32) || defined(ARM_32)
typedef uint32_t addr_t;
#else
typedef uint64_t addr_t; // TODO add cases that enter here
#endif

typedef uint32_t threadid_t;

#if defined(TARGET_IA32) || defined(ARM_32)
typedef uint32_t syscall_args_t[6];
#else
typedef uint64_t syscall_args_t[6];
#endif

typedef struct
{
  Sift::Writer* output;
  uint64_t dyn_addresses[Sift::MAX_DYNAMIC_ADDRESSES];
  uint32_t num_dyn_addresses;
  Bbv* bbv;
  uint64_t thread_num;
  addr_t bbv_base;
  uint64_t bbv_count;
  addr_t bbv_last;
  bool bbv_end;
  uint64_t blocknum;
  uint64_t icount;
  uint64_t icount_cacheonly;
  uint64_t icount_cacheonly_pending;
  uint64_t icount_detailed;
  uint64_t icount_reported;
  addr_t last_syscall_number;
  addr_t last_syscall_returnval;
  uint64_t flowcontrol_target;
  addr_t tid_ptr;
  addr_t last_routine;
  addr_t last_call_site;
  bool last_syscall_emulated;
  bool running;
  bool should_send_threadinfo;
} __attribute__((packed, aligned(LINE_SIZE_BYTES))) thread_data_t;

// enums

/// ISAs supported in the frontend
enum FrontendISA 
{
  UNDEF_ISA,
  INTEL_IA32, 
  INTEL_X86_64, 
  ARM_AARCH32, 
  ARM_AARCH64
};  

#endif // __FRONTEND_DEFS_H