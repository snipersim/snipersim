#include "timed_calls.h"
#include "threads.h"
#include "guest_time.h"

#include <syscall.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <vector>

/* No time the guest gives reaches the host's kernel (sift/guest_time.h).
 *
 * The calls the simulator models (futex, the sleeps) it times itself, on a
 * live channel (syscall_modeling.cc).  Every other timed wait is run here,
 * in the analysis routine before the system call instruction, which is then
 * skipped as an emulated call is: the guest's process is this one, so the
 * calls are made with timeouts of the recorder's own, never the guest's --
 *
 * - live, as probes that do not wait (a zero timeout), between which the
 *   simulator lets the thread sleep in simulated time (a nanosleep it
 *   models, of 1 us doubling to 1 ms), until the guest's deadline in
 *   simulated time;
 * - where the simulator does not answer (a trace written to a file), in
 *   slices of SLICE_NS of host time that only let the recorder look again,
 *   until the one-IPC model says the deadline has passed.
 *
 * A signal that arrives during such a wait ends it with EINTR.  The guest's
 * system calls are x86-64's; the i386 recorder leaves them alone. */

#if defined(TARGET_INTEL64) || defined(TARGET_IA32E)
# define TIMED_CALLS 1
#else
# define TIMED_CALLS 0
#endif

namespace
{

struct PinLock
{
   PIN_LOCK l;
   PinLock() { PIN_InitLock(&l); }
   void lock() { PIN_GetLock(&l, 0); }
   void unlock() { PIN_ReleaseLock(&l); }
};

UINT64 clock_instructions(unsigned slot, void *)
{
   return thread_data[slot].icount_total;
}

typedef Sift::GuestClocks<PinLock> Clocks;
Clocks *clocks = NULL;

/* the threads in a call that can block, where the simulator does not answer
   (each written by its own thread only) */
bool *waiting = NULL;

const UINT64 SLICE_NS = 1000000;
const UINT64 LIVE_SLEEP_MIN = 1000, LIVE_SLEEP_MAX = 1000000;

bool live(THREADID threadid)
{
   return thread_data[threadid].output && KnobEmulateSyscalls.Value();
}

/* The system call itself: PinCRT's syscall() refuses some six-argument
   calls (pselect6 and epoll_pwait came back ENOSYS) */
long host_syscall(ADDRINT num, const UINT64 a[6])
{
#if TIMED_CALLS
   long r;
   register UINT64 r10 __asm__("r10") = a[3];
   register UINT64 r8 __asm__("r8") = a[4];
   register UINT64 r9 __asm__("r9") = a[5];
   __asm__ volatile("syscall" : "=a"(r) : "a"(num), "D"(a[0]), "S"(a[1]), "d"(a[2]), "r"(r10), "r"(r8), "r"(r9)
                    : "rcx", "r11", "memory");
   return r;
#else
   (void)num; (void)a;
   return -ENOSYS;
#endif
}

bool read_mem(ADDRINT addr, void *dst, size_t size)
{
   return PIN_SafeCopy(dst, (const void *)addr, size) == size;
}

bool write_mem(ADDRINT addr, const void *src, size_t size)
{
   return PIN_SafeCopy((void *)addr, src, size) == size;
}

void put_time(struct timespec *ts, UINT64 ns)
{
   ts->tv_sec = ns / Sift::NS_PER_S;
   ts->tv_nsec = ns % Sift::NS_PER_S;
}

/* The time a timed call ends at (see the QEMU plugin's call_deadline) */
bool call_deadline(const Sift::TimedCall &tc, const syscall_args_t &args, UINT64 now, UINT64 cpu_now,
                   UINT64 *deadline, long *err)
{
   UINT64 v;
   if (tc.form == Sift::TO_MS)
      v = (UINT64)(INT64)(INT32)args[tc.arg] * 1000000ull;
   else
   {
      INT64 w[2];
      if (!read_mem(args[tc.arg], w, sizeof(w)))
      {
         *err = -EFAULT;
         return false;
      }
      const INT64 frac_max = tc.form == Sift::TO_TIMEVAL ? 1000000 : 1000000000;
      if (w[0] < 0 || w[1] < 0 || w[1] >= frac_max)
      {
         *err = -EINVAL;
         return false;
      }
      const UINT64 sec = (UINT64)w[0] > 9000000000ull ? 9000000000ull : (UINT64)w[0];
      v = sec * Sift::NS_PER_S + (UINT64)w[1] * (tc.form == Sift::TO_TIMEVAL ? 1000 : 1);
   }
   if (!tc.absolute)
      *deadline = now + v;
   else if (Sift::cputime_clock(tc.clock))
      *deadline = now + (v > cpu_now ? v - cpu_now : 0);
   else
   {
      const UINT64 zero = Sift::TIME_START_DEFAULT * Sift::NS_PER_S;
      *deadline = v > zero ? v - zero : 0;
   }
   return true;
}

/* The guest's arguments with the timeout replaced: ns of waiting (0: a
   probe), in *own (a timespec or timeval of the recorder's) */
void own_timeout(const Sift::TimedCall &tc, UINT64 a[6], UINT64 ns, struct timespec *own)
{
   switch (tc.form)
   {
      case Sift::TO_MS:
         a[tc.arg] = ns ? (ns + 999999) / 1000000 : 0;
         return;
      case Sift::TO_TIMEVAL:
      {
         struct timeval *tv = (struct timeval *)own;
         tv->tv_sec = ns / Sift::NS_PER_S;
         tv->tv_usec = ns % Sift::NS_PER_S / 1000;
         a[tc.arg] = (UINT64)tv;
         return;
      }
      default:
         if (tc.absolute)
         {
            UINT64 at = 0;   /* the epoch: past */
            if (ns)
            {
               struct timespec t;
               clock_gettime(tc.clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &t);
               at = (UINT64)t.tv_sec * Sift::NS_PER_S + (UINT64)t.tv_nsec + ns;
            }
            put_time(own, at);
         }
         else
            put_time(own, ns);
         a[tc.arg] = (UINT64)own;
   }
}

long timeout_result(int x86)
{
   switch (x86)
   {
      case Sift::X86_futex: case Sift::X86_futex_waitv:
      case Sift::X86_mq_timedsend: case Sift::X86_mq_timedreceive:
         return -ETIMEDOUT;
      case Sift::X86_rt_sigtimedwait: case Sift::X86_semtimedop:
         return -EAGAIN;
      default:
         return 0;
   }
}

struct FdSets
{
   std::vector<UINT8> set[3];
   bool have[3];
   FdSets() { have[0] = have[1] = have[2] = false; }
   bool save(const syscall_args_t &args)
   {
      const UINT64 n = args[0];
      if (n > (1u << 20))
         return false;
      const size_t bytes = (n + 63) / 64 * 8;
      for (int i = 0; i < 3; i++)
      {
         have[i] = args[1 + i] != 0 && bytes;
         if (have[i])
         {
            set[i].resize(bytes);
            if (!read_mem(args[1 + i], &set[i][0], bytes))
               return false;
         }
      }
      return true;
   }
   void restore(const syscall_args_t &args) const
   {
      for (int i = 0; i < 3; i++)
         if (have[i])
            write_mem(args[1 + i], &set[i][0], set[i].size());
   }
};

/* the time left, as the kernel leaves it (see the QEMU plugin's time_left) */
void time_left(const Sift::TimedCall &tc, const syscall_args_t &args, int x86, UINT64 deadline, UINT64 now, long ret)
{
   const UINT64 left = deadline != Sift::NO_DEADLINE && deadline > now ? deadline - now : 0;
   if ((x86 == Sift::X86_select || x86 == Sift::X86_pselect6 || x86 == Sift::X86_ppoll)
       && args[tc.arg] && deadline != Sift::NO_DEADLINE)
   {
      const UINT64 w[2] = { left / Sift::NS_PER_S, x86 == Sift::X86_select ? left % Sift::NS_PER_S / 1000 : left % Sift::NS_PER_S };
      write_mem(args[tc.arg], w, sizeof(w));
   }
   if (tc.cls == Sift::TC_SLEEP && ret == -EINTR && !tc.absolute && tc.rem_arg >= 0 && args[tc.rem_arg])
   {
      const UINT64 w[2] = { left / Sift::NS_PER_S, left % Sift::NS_PER_S };
      write_mem(args[tc.rem_arg], w, sizeof(w));
   }
}

bool recvmmsg_untimed(const syscall_args_t &args, ADDRINT *ret)
{
   UINT64 a[6];
   memcpy(a, args, sizeof(a));
   a[3] |= 0x10000;   /* MSG_WAITFORONE: see the QEMU plugin's recvmmsg_untimed */
   a[4] = 0;
   *ret = (ADDRINT)host_syscall(Sift::X86_recvmmsg, a);
   return true;
}

/* The one-IPC model's timed wait */
bool timed_one_ipc(THREADID threadid, int x86, const syscall_args_t &args, const Sift::TimedCall &tc, ADDRINT *ret)
{
   const UINT64 word = x86 == Sift::X86_futex ? args[0] : 0;
   if (x86 == Sift::X86_recvmmsg && args[4])
   {
      clocks->wait_begin(threadid, Sift::NO_DEADLINE, 0);
      recvmmsg_untimed(args, ret);
      clocks->wait_end(threadid, Clocks::WOKEN);
      return true;
   }
   if (Sift::timeout_infinite(tc, args))
   {
      if (tc.cls == Sift::TC_SLEEP)
      {
         *ret = (ADDRINT)-EFAULT;
         return true;
      }
      clocks->wait_begin(threadid, Sift::NO_DEADLINE, word);
      waiting[threadid] = true;
      return false;
   }
   const UINT64 now = clocks->now(threadid);
   UINT64 deadline;
   long err = 0;
   if (!call_deadline(tc, args, now, clocks->process_cpu(), &deadline, &err))
   {
      *ret = (ADDRINT)err;
      return true;
   }
   FdSets sets;
   if (tc.fdsets && !sets.save(args))
   {
      *ret = (ADDRINT)-EFAULT;
      return true;
   }

   clocks->wait_begin(threadid, deadline, word);
   Clocks::End how = Clocks::WOKEN;
   long r = 0;
   bool idle_seen = false;
   UINT64 idle_epoch = 0;
   struct timespec own;
   for (;;)
   {
      bool expire = deadline <= now;
      if (!expire)
      {
         UINT64 epoch;
         const int m = clocks->may_expire(threadid, &epoch);
         expire = m == 1 || (m == 2 && idle_seen && epoch == idle_epoch);
         idle_seen = m == 2;
         idle_epoch = epoch;
      }
      UINT64 a[6];
      memcpy(a, args, sizeof(a));
      if (tc.cls == Sift::TC_SLEEP)
      {
         if (expire)
         {
            how = Clocks::TIMED_OUT;
            r = 0;
            break;
         }
         put_time(&own, SLICE_NS);
         const UINT64 s[6] = { (UINT64)&own, 0, 0, 0, 0, 0 };
         r = host_syscall(SYS_nanosleep, s);
         if (r == 0)
            continue;
         how = Clocks::INTERRUPTED;
         break;
      }
      sets.restore(args);
      own_timeout(tc, a, expire ? 0 : SLICE_NS, &own);
      r = host_syscall(x86, a);
      if (r == timeout_result(x86))
      {
         if (expire)
         {
            how = Clocks::TIMED_OUT;
            break;
         }
         continue;
      }
      how = r == -EINTR ? Clocks::INTERRUPTED : Clocks::WOKEN;
      break;
   }
   clocks->wait_end(threadid, how);
   time_left(tc, args, x86, deadline, clocks->now(threadid), r);
   *ret = (ADDRINT)r;
   return true;
}

/* The simulator's time, without the clocks' zero */
bool sim_time(THREADID threadid, UINT64 *ns)
{
   Sift::EmuRequest req;
   Sift::EmuReply res;
   if (!thread_data[threadid].output->Emulate(Sift::EmuTypeCounters, req, res))
      return false;
   *ns = res.counters.time_ns;
   return true;
}

/* A live timed wait the simulator does not model */
bool timed_live(THREADID threadid, int x86, const syscall_args_t &args, const Sift::TimedCall &tc, ADDRINT *ret)
{
   if (tc.cls == Sift::TC_SLEEP || x86 == Sift::X86_futex)
      return false;                 /* the simulator's (syscall_modeling.cc) */
   if (x86 == Sift::X86_futex_waitv)
   {
      *ret = (ADDRINT)-ENOSYS;      /* a futex wait outside the simulator's model of the futexes */
      return true;
   }
   if (x86 == Sift::X86_recvmmsg)
      return args[4] && recvmmsg_untimed(args, ret);
   UINT64 now;
   if (!sim_time(threadid, &now))
      return false;
   UINT64 deadline = Sift::NO_DEADLINE;
   long err = 0;
   if (!Sift::timeout_infinite(tc, args) && !call_deadline(tc, args, now, now, &deadline, &err))
   {
      *ret = (ADDRINT)err;
      return true;
   }
   FdSets sets;
   if (tc.fdsets && !sets.save(args))
   {
      *ret = (ADDRINT)-EFAULT;
      return true;
   }
   UINT64 sleep = LIVE_SLEEP_MIN;
   long r = 0;
   struct timespec own, nap;
   for (;;)
   {
      UINT64 a[6];
      memcpy(a, args, sizeof(a));
      sets.restore(args);
      own_timeout(tc, a, 0, &own);
      r = host_syscall(x86, a);
      if (r != timeout_result(x86) || (deadline != Sift::NO_DEADLINE && now >= deadline))
         break;
      UINT64 s = sleep;
      if (deadline != Sift::NO_DEADLINE && deadline - now < s)
         s = deadline - now;
      put_time(&nap, s);
      syscall_args_t sa = { (UINT64)&nap, 0, 0, 0, 0, 0 };
      thread_data[threadid].output->Syscall(SYS_nanosleep, (char *)sa, sizeof(sa));
      if (!sim_time(threadid, &now))
         break;
      sleep = sleep * 2 < LIVE_SLEEP_MAX ? sleep * 2 : LIVE_SLEEP_MAX;
   }
   time_left(tc, args, x86, deadline, now, r);
   *ret = (ADDRINT)r;
   return true;
}

bool can_block(int x86, const syscall_args_t &args)
{
   switch (x86)
   {
      case 0: case 17: case 19: case 45: case 47: case 43: case 288: case 42: case 61: case 247:
      case 70: case 69: case 65: case 34: case 130: case 73:
      case 1: case 20: case 44: case 46: case 307:
         return true;
      case Sift::X86_futex:
      {
         bool absolute;
         int clock;
         return Sift::futex_timed_op(args[1], &absolute, &clock);
      }
      default:
         return false;
   }
}

void note_wakes(THREADID threadid, int x86, const syscall_args_t &args)
{
   switch (x86)
   {
      case 1: case 18: case 20: case 44: case 46: case 307: case 3:
      case 62: case 200: case 234: case 129: case 297:
         clocks->woke(threadid, 0);
         break;
      case Sift::X86_futex:
         switch (args[1] & 127)
         {
            case 1: case 7: case 10:
               clocks->woke(threadid, args[0]);
               break;
            case 3: case 4: case 5: case 12:
               clocks->woke(threadid, args[0]);
               clocks->woke(threadid, args[4]);
               break;
         }
         break;
   }
}

}  // namespace

void timedInit()
{
   clocks = new Clocks(max_num_threads, clock_instructions, NULL);
   waiting = new bool[max_num_threads]();
}

void timedThreadStart(THREADID threadid, INT32 parent)
{
   waiting[threadid] = false;
   clocks->start(threadid, parent);
}

void timedThreadExit(THREADID threadid)
{
   clocks->exit(threadid, thread_data[threadid].tid_ptr);
}

UINT64 timedNow(THREADID threadid) { return clocks->now(threadid); }
UINT64 timedThreadCpu(THREADID threadid) { return clocks->thread_cpu(threadid); }
UINT64 timedProcessCpu() { return clocks->process_cpu(); }

bool timedSyscall(THREADID threadid, ADDRINT num, const syscall_args_t &args, ADDRINT *ret)
{
   if (!TIMED_CALLS || replayingPinball())
      return false;
   const int x86 = (int)num;
   Sift::TimedCall tc;
   const bool timed = Sift::timed_call(x86, args, &tc);
   if (live(threadid))
      return timed && timed_live(threadid, x86, args, tc, ret);
   note_wakes(threadid, x86, args);
   if (timed)
      return timed_one_ipc(threadid, x86, args, tc, ret);
   if (can_block(x86, args))
   {
      clocks->wait_begin(threadid, Sift::NO_DEADLINE, x86 == Sift::X86_futex ? args[0] : 0);
      waiting[threadid] = true;
   }
   return false;
}

void timedSyscallReturned(THREADID threadid, ADDRINT ret)
{
   if (!waiting || !waiting[threadid])
      return;
   waiting[threadid] = false;
   clocks->wait_end(threadid, (long)ret == -EINTR ? Clocks::INTERRUPTED : Clocks::WOKEN);
}
