#include "logmem.h"
#include "simulator.h"
#include "lock.h"
#include "timer.h"
#include "hooks_manager.h"

#include <cstdio>

/*
   Memory Logger
   Logs allocated memory size per allocation site

   Usage:
   - define LOGMEM_ENABLED in logmem.h
   - recompile all sources with DEBUG=1
   - run simulation
     + to completion (ROI-end)
     + send USR1 signal for intermediate statistics
   - allocations will be in allocations.out
   - ./tools/memtop.py
*/

#ifndef LOGMEM_ENABLED

void logmem_enable(bool enabled) {}
void logmem_write_allocations() {}

#else

const int NUM_ITEMS = 1024*1024;

#include "callstack.h"
#include <execinfo.h>
class AllocItem
{
   static const unsigned int BACKTRACE_SIZE = 15;
   void * backtrace_buffer[BACKTRACE_SIZE];
   unsigned int backtrace_n;
   static Lock lock;
public:
   size_t size;
   size_t count;
   AllocItem() : backtrace_n(0), size(0), count(0) {}
   static unsigned int key() {
      void * buffer[BACKTRACE_SIZE];
      // use the fast version here
      int n = get_call_stack(buffer, 6);
      unsigned int k = 0;
      for(int i = 2; i < n; ++i)
         k += (unsigned long)buffer[i] * (i + (1 << i));
      return k % NUM_ITEMS;
   }
   void init() {
      ScopedLock sl(lock);
      backtrace_n = backtrace(backtrace_buffer, BACKTRACE_SIZE);
   }
   void record(size_t _size) {
      if (backtrace_n == 0) init();
      __sync_fetch_and_add(&size, _size);
      __sync_fetch_and_add(&count, 1);
   }
   void free(size_t _size) {
      __sync_fetch_and_sub(&size, _size);
   }
   void report(FILE * fp) {
      for(unsigned int i = 3; i < BACKTRACE_SIZE; ++i)
        fprintf(fp, "%lu ", i < backtrace_n ? (unsigned long)backtrace_buffer[i] : 0);
      fprintf(fp, "%lu %lu\n", (unsigned long)size, (unsigned long)count);
   }
};

struct DataItem
{
   unsigned int key;
   size_t size;
   char data[];
};

AllocItem allocated[NUM_ITEMS];
bool logmem_enabled = false;      // global enable (during ROI for instance)
SInt64 mem_total = 0;
Lock AllocItem::lock;

void * __new(size_t size)
{
   DataItem *p = (DataItem*)malloc(sizeof(DataItem) + size);
   p->key = -1;
   p->size = size;
   if (logmem_enabled) {
      unsigned int k = AllocItem::key();
      p->key = k;
      allocated[k].record(size);
      __sync_fetch_and_add(&mem_total, size);
   }
   return p->data;
}

void * operator new (size_t size) { return __new(size); }
void * operator new [] (size_t size) { return __new(size); }

void __delete(void* p)
{
   DataItem *_p = (DataItem*)(((char*)p) - sizeof(DataItem));
   if (logmem_enabled) {
      if (_p->key != (unsigned int)-1) {
         allocated[_p->key].free(_p->size);
         __sync_fetch_and_sub(&mem_total, _p->size);
      }
   }
   free(_p);
}

void operator delete(void* p) { __delete(p); }
void operator delete [] (void* p) { __delete(p); }


/* Output format:
   First line: address of rdtsc (first argument of tools/addr2line.py)
   Subsequent lines: backtrace addresses, last element is size
   Postprocess step: sort allocations.out | uniq -c | sort -n
*/
void logmem_write_allocations()
{
   printf("[LOGMEM] Total memory consumption increase: %.1f MiB\n", mem_total / (1024.*1024));
   FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("allocations.out").c_str(), "w");
   fprintf(fp, "%lu\n", (unsigned long)rdtsc);
   for(int i = 0; i < NUM_ITEMS; ++i)
      if (allocated[i].size > 0)
         allocated[i].report(fp);
   fclose(fp);
}

SInt64 logmem_trigger(UInt64, UInt64)
{
   printf("[SNIPER] Writing logmem allocations\n");
   logmem_write_allocations();
   return 0;
}

void logmem_enable(bool enabled)
{
   if (!logmem_enabled)
   {
      // let's see whether get_call_stack() is working
      // if -fomit-frame-pointer was used, we should notice this here rather than working with garbage keys
      const unsigned int BACKTRACE_SIZE = 5;
      void *backtrace_buffer[BACKTRACE_SIZE], *callstack_buffer[BACKTRACE_SIZE];
        unsigned int backtrace_n = backtrace(backtrace_buffer, BACKTRACE_SIZE),
                     callstack_n = get_call_stack(callstack_buffer, BACKTRACE_SIZE);
      for(unsigned int i = 1; i < backtrace_n; ++i)
      {
         LOG_ASSERT_ERROR(i < callstack_n && backtrace_buffer[i] == callstack_buffer[i], "Fast backtrace() not working");
      }

      Sim()->getHooksManager()->registerHook(HookType::HOOK_SIGUSR1, logmem_trigger, 0);
   }
   logmem_enabled = enabled;
}

#endif // LOGMEM_ENABLED
