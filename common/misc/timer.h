#ifndef TIMER_H
#define TIMER_H

#include "fixed_point.h"

#include <sys/time.h>
#include <vector>

UInt64 rdtsc(void);

class Timer
{
   public:
      bool switched;

      Timer();
      ~Timer();

      /** Start timing */
      void start(void);
      /** Return elapsed time in nanoseconds */
      UInt64 getTime(void);
      static UInt64 now(void);

   private:
      UInt64 t_start;
      UInt64 r_start;
      UInt32 cpu_start;
      // Default FixedPoint uses a 16k multiplier, this overflows with long simulation times.
      // Now, we can simulate at least 3 months (0x100 * rdtsc() < INT64_MAX) with sufficient precision
      typedef TFixedPoint<0x100> RdtscSpeed;
      static RdtscSpeed rdtsc_speed;
};

class TotalTimer
{
   public:
      TotalTimer(String _name, UInt32 _ignore = 0);
      ~TotalTimer();

      void add(UInt64 t, bool switched = false) {
         __sync_fetch_and_add(&total, t);
         __sync_fetch_and_add(&n, 1);
         if (t > max)
            max = t;
         if (switched)
            __sync_fetch_and_add(&n_switched, 1);
      }
      void report(FILE* fp);

      static void reports(void);
      static TotalTimer* getTimerByStacktrace(String name);

   private:
      String name;
      UInt64 total;
      UInt64 n;
      UInt64 max;
      UInt64 n_switched;

      static const unsigned long BACKTRACE_SIZE = 8;
      void * backtrace_buffer[BACKTRACE_SIZE];
      unsigned long backtrace_n, backtrace_ignore;
};

class ScopedTimer
{
   private:
      TotalTimer &total;
      Timer timer;

   public:
      ScopedTimer(TotalTimer &_total)
         : total(_total)
      {
      }

      ~ScopedTimer()
      {
         UInt64 t = timer.getTime();
         total.add(t, timer.switched);
      }
};

#endif // TIMER_H
