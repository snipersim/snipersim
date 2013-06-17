#include "timer.h"
#include "simulator.h"

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Define to handle processor switches using cpuid and a clock_gettime() fallback when a switch is detected.
// By default, only the faster rdtsc is used, but this can give errors when a thread switches cores while timing.
//#define TIMER_TRACK_CPUID

Timer::RdtscSpeed Timer::rdtsc_speed = 0;

UInt64 rdtsc(void)
{
    UInt32 lo, hi;
    __asm__ __volatile__ (
      "rdtsc\n"
      : "=a" (lo), "=d" (hi)
      :
      : "%ebx", "%ecx" );
    return (UInt64)hi << 32 | lo;
}

inline UInt64 rdtsc_and_cpuid(UInt32 *id)
{
    UInt32 lo, hi;
    __asm__ __volatile__ (
      "mov $0xb, %%eax\n"
      "cpuid\n"
      "mov %%edx, %0\n"
      "rdtsc\n"
      : "=r" (*id), "=a" (lo), "=d" (hi)
      :
      : "%ebx", "%ecx" );
    return (UInt64)hi << 32 | lo;
}

Timer::Timer()
{
   if (rdtsc_speed == 0) {
      UInt32 id1, id2;
      do {
         UInt64 t_start = now(), r_start = rdtsc_and_cpuid(&id1);
         usleep(100000);
         UInt64 t_end = now(), r_end = rdtsc_and_cpuid(&id2);
         rdtsc_speed = RdtscSpeed(r_end - r_start) / (t_end - t_start);
      } while (id1 != id2);
   }
   start();
}

Timer::~Timer()
{
}

UInt64 Timer::now()
{
   timespec t;
   clock_gettime(CLOCK_REALTIME, &t);
   return (UInt64(t.tv_sec) * 1000000000) + t.tv_nsec;
}

void Timer::start()
{
   #ifdef TIMER_TRACK_CPUID
   t_start = now();
   r_start = rdtsc_and_cpuid(&cpu_start);
   #else
   t_start = rdtsc();
   #endif
   switched = false;
}

UInt64 Timer::getTime()
{
   #ifdef TIMER_TRACK_CPUID
   UInt32 cpu_now;
   UInt64 r_now = rdtsc_and_cpuid(&cpu_now);

   if (cpu_now == cpu_start)
      return RdtscSpeed::floor((r_now - r_start) / rdtsc_speed);
   else {
      switched = true;
      return now() - t_start;
   }
   #else
   return RdtscSpeed::floor((rdtsc() - t_start) / rdtsc_speed);
   #endif
}


/* Don't use a std::vector or anything fancy here, as this may be initialized *after*
   one of the timers, removing them from the list */
static const int MAX_TIMERS = 1024;
static TotalTimer* alltimers[MAX_TIMERS];
static int numtimers = 0;

TotalTimer::TotalTimer(String _name, UInt32 _ignore)
   : name(_name), total(0), n(0), max(0), n_switched(0), backtrace_ignore(_ignore)
{
   if (numtimers < MAX_TIMERS)
      alltimers[numtimers++] = this;
   backtrace_n = backtrace(backtrace_buffer, BACKTRACE_SIZE);
}

TotalTimer::~TotalTimer()
{
}

void TotalTimer::report(FILE* fp)
{
   //printf("[TIMER] <%-20s> %6.1f s, %lu calls, avg %.0f us/call, max %.0f us, switched = %.3f%% (%lu) ",
   //   name.c_str(), total / 1e9, n, total / (n ? n : 1) / 1e3, max / 1e3, 100. * n_switched / n, n_switched);
   fprintf(fp, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64, total, n, max, n_switched);
   for(unsigned long i = backtrace_ignore; i < backtrace_ignore + 6; ++i)
     fprintf(fp, " %" PRIuPTR, i < backtrace_n ? (intptr_t)backtrace_buffer[i] : 0);
   fprintf(fp, " %s\n", name.c_str());
}

void TotalTimer::reports(void)
{
   if (numtimers > 0) {
      char * timersfile = strdup(Sim()->getConfig()->formatOutputFileName("sim_timers.out").c_str());
      FILE* fp = fopen(timersfile, "w");
      fprintf(fp, "%" PRIuPTR "\n", (intptr_t)rdtsc);
      for(int i = 0; i < numtimers; ++i)
         alltimers[i]->report(fp);
      fclose(fp);
   }
}


static UInt64 getHashByStacktrace(void)
{
   const int SIZE = 12;
   void * buffer[SIZE];
   int n = backtrace(buffer, SIZE);
   UInt64 hash = 1861;
   for(int i = 0; i < n; ++i)
      hash = ((hash * __UINT64_C(1994945959)) + (UInt64)buffer[i]) % __UINT64_C(49492920901);
   return hash;
}

/* Initialize manually, as some objects that are also static use us but may be initialized earlier. */
static std::map<UInt64, TotalTimer*> * totaltimershash = NULL;

TotalTimer* TotalTimer::getTimerByStacktrace(String name)
{
   if (! totaltimershash)
      totaltimershash = new std::map<UInt64, TotalTimer*>();

   UInt64 hash = getHashByStacktrace();
   if (totaltimershash->count(hash) == 0)
      (*totaltimershash)[hash] = new TotalTimer(name, 1);
   return totaltimershash->at(hash);
}
