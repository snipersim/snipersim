#include "logmem.h"
#include "simulator.h"
#include "lock.h"

#include <stdio.h>
#include <unordered_map>
#include <sys/syscall.h>


#ifndef LOGMEM_ENABLED

void logmem_enable(bool enabled) {}
void logmem_write_allocations() {}

#else

#include <execinfo.h>
class AllocItem
{
   static const unsigned int BACKTRACE_SIZE = 10;
   void * backtrace_buffer[BACKTRACE_SIZE];
   unsigned int backtrace_n;
public:
   size_t size;
   AllocItem(size_t _size) {
      size = _size;
      backtrace_n = backtrace(backtrace_buffer, BACKTRACE_SIZE);
   }
   void report(FILE * fp) {
      for(unsigned int i = 3; i < BACKTRACE_SIZE; ++i)
        fprintf(fp, "%lu ", i < backtrace_n ? (unsigned long)backtrace_buffer[i] : 0);
      fprintf(fp, "%lu\n", (unsigned long)size);
   }
};

std::unordered_map<void *, AllocItem *> allocated;
bool logmem_enabled = false;      // global enable (during ROI for instance)
int logmem_bookkeeping = 0;  // temporary disable (while doing bookkeeping)
SInt64 mem_total = 0;
Lock l_alloc;

int gettid() { return syscall(__NR_gettid); }

void * __new(size_t size)
{
   if (logmem_enabled && logmem_bookkeeping != gettid()) {
      ScopedLock sl(l_alloc);
      logmem_bookkeeping = gettid();
      void *p = malloc(size);
      if (allocated.count(p)) {
         AllocItem * it = allocated[p];
         mem_total -= it->size;
         delete it;
      }
      allocated[p] = new AllocItem(size);
      mem_total += size;
      logmem_bookkeeping = 0;
      return p;
   } else
      return malloc(size);
}

void * operator new (size_t size) { return __new(size); }
void * operator new [] (size_t size) { return __new(size); }

void __delete(void* p)
{
   if (logmem_enabled && logmem_bookkeeping != gettid()) {
      ScopedLock sl(l_alloc);
      logmem_bookkeeping = gettid();
      free(p);
      if (allocated.count(p)) {
         AllocItem * it = allocated[p];
         mem_total -= it->size;
         delete it;
         allocated.erase(p);
      }
      logmem_bookkeeping = 0;
   } else
      free(p);
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
   printf("[LOGMMEM] Total memory consumption increase: %.1f MiB\n", mem_total / (1024.*1024));
   FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("allocations.out").c_str(), "w");
   fprintf(fp, "%lu\n", (unsigned long)rdtsc);
   for(std::unordered_map<void *, AllocItem *>::iterator it = allocated.begin(); it != allocated.end(); ++it)
      it->second->report(fp);
   fclose(fp);
}

void logmem_enable(bool enabled)
{
   logmem_enabled = enabled;
}

#endif // LOGMEM_ENABLED
