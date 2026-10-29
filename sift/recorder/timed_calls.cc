#include "timed_calls.h"
#include "threads.h"
#include "guest_time.h"

#include <syscall.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <vector>
#include <signal.h>
#include <fcntl.h>
#include <sys/timerfd.h>

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
      const UINT64 zero = guestTimeZeroNs();
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

unsigned timers_check(INT32 threadid);   /* the timers, below */
UINT64 process_cpu_now(INT32 threadid);
extern volatile UINT64 timers_fired;

/* A wait a timer's signal ended.  Pin delivers a signal that arrives in the
   analysis routine before a system call by running the guest's handler and
   then executing the system call instruction again (its rt_sigreturn in
   between): the call would start over -- a pause never returned, its
   handler running at every timer.  The thread's next call, if it is the
   same one, returns EINTR at once. */
struct Interrupted { bool valid; ADDRINT num; UINT64 args[6]; };
Interrupted *interrupted = NULL;

void mark_interrupted(THREADID threadid, ADDRINT num, const syscall_args_t &args)
{
   interrupted[threadid].valid = true;
   interrupted[threadid].num = num;
   memcpy(interrupted[threadid].args, args, sizeof(interrupted[threadid].args));
}

/* A thread in a sigtimedwait the recorder runs, and the signals it waits
   for: a timer's signal among them is handed to it here, with its siginfo,
   rather than sent.  Sent, Pin would take it for the guest -- the recorder's
   own sigtimedwait in the analysis routine never sees it, and Pin's handling
   of the guest's call did not either. */
struct SigWait { volatile bool waiting, got; UINT64 set; siginfo_t info; };
SigWait *sigwaits = NULL;
PIN_LOCK sigwait_lock;

void sigwait_begin(THREADID threadid, const syscall_args_t &args)
{
   UINT64 set = 0;
   read_mem(args[0], &set, args[3] < sizeof(set) ? args[3] : sizeof(set));
   PIN_GetLock(&sigwait_lock, 0);
   sigwaits[threadid].set = set;
   sigwaits[threadid].got = false;
   sigwaits[threadid].waiting = true;
   PIN_ReleaseLock(&sigwait_lock);
}

/* The signal handed to the waiter (its number, its siginfo written), or 0 */
long sigwait_take(THREADID threadid, const syscall_args_t &args)
{
   if (!sigwaits[threadid].got)
      return 0;
   if (args[1])
      write_mem(args[1], &sigwaits[threadid].info, sizeof(siginfo_t));
   return sigwaits[threadid].info.si_signo;
}

void sigwait_end(THREADID threadid)
{
   PIN_GetLock(&sigwait_lock, 0);
   sigwaits[threadid].waiting = sigwaits[threadid].got = false;
   PIN_ReleaseLock(&sigwait_lock);
}

/* A timer's signal to a waiter whose set has it: true when handed over */
bool sigwait_give(const siginfo_t &info)
{
   bool given = false;
   PIN_GetLock(&sigwait_lock, 0);
   for (unsigned i = 0; i < max_num_threads && !given; i++)
      if (sigwaits[i].waiting && !sigwaits[i].got && (sigwaits[i].set >> (info.si_signo - 1)) & 1)
      {
         sigwaits[i].info = info;
         sigwaits[i].got = true;
         given = true;
      }
   PIN_ReleaseLock(&sigwait_lock);
   return given;
}

bool restarted_after_signal(THREADID threadid, ADDRINT num, const syscall_args_t &args)
{
   if (num == SYS_rt_sigreturn)
      return false;   /* the handler's return, between the two */
   Interrupted &i = interrupted[threadid];
   const bool match = i.valid && i.num == num && !memcmp(i.args, args, sizeof(i.args));
   i.valid = false;
   return match;
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
   if (!call_deadline(tc, args, now, process_cpu_now(-1), &deadline, &err))
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
   const bool sigwait = x86 == Sift::X86_rt_sigtimedwait;
   if (sigwait)
      sigwait_begin(threadid, args);
   for (;;)
   {
      timers_check(threadid);
      if (sigwait && (r = sigwait_take(threadid, args)) != 0)
         break;                     /* a timer's signal, handed over */
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
   if (sigwait)
      sigwait_end(threadid);
   clocks->wait_end(threadid, how);
   time_left(tc, args, x86, deadline, clocks->now(threadid), r);
   *ret = (ADDRINT)r;
   return true;
}

/* The simulator's time, without the clocks' zero */
/* Live, the simulator's CPU times (EmuTypeCpuTime): the thread's and its
   process's; false without a channel or from an older simulator (the
   one-IPC model's then) */
bool sim_cpu_times(THREADID threadid, UINT64 *thread_ns, UINT64 *process_ns)
{
   if (!live(threadid))
      return false;
   Sift::EmuRequest req;
   Sift::EmuReply res;
   req.cputime.clock = 2;
   if (!thread_data[threadid].output->Emulate(Sift::EmuTypeCpuTime, req, res))
      return false;
   if (thread_ns)
      *thread_ns = res.cputime.thread_ns;
   if (process_ns)
      *process_ns = res.cputime.process_ns;
   return true;
}

UINT64 process_cpu_now(INT32 threadid)
{
   UINT64 p;
   if (threadid >= 0 && sim_cpu_times(threadid, NULL, &p))
      return p;
   return clocks->process_cpu();
}

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
   if (x86 == Sift::X86_rt_sigtimedwait)
      sigwait_begin(threadid, args);
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
      if (timers_check(threadid))
      {
         if (x86 == Sift::X86_rt_sigtimedwait)
         {
            const long s = sigwait_take(threadid, args);
            if (s)
            {
               r = s;               /* the timer's signal, handed over */
               break;
            }
            continue;               /* someone else's */
         }
         /* a signal for the guest: the wait ends with EINTR (see Interrupted) */
         mark_interrupted(threadid, x86, args);
         r = -EINTR;
         break;
      }
   }
   if (x86 == Sift::X86_rt_sigtimedwait)
      sigwait_end(threadid);
   time_left(tc, args, x86, deadline, now, r);
   *ret = (ADDRINT)r;
   return true;
}


/* ---------------------------------------------------------------- timers
 *
 * alarm, setitimer, the POSIX timers and timerfd in the guest's time
 * (Sift::GuestTimers; see the QEMU plugin's timers): kept here, fired when
 * the guest's time passes their expiry -- the simulator's, live, else the
 * time every thread has reached in the one-IPC model -- with calls that
 * carry no time (rt_sigqueueinfo, as the kernel's signal; the timerfd itself
 * armed to expire at once).  They are looked at in every system call, in
 * the wait loops, live at flow control every TIMER_CHECK instructions, and
 * without a simulator by an internal thread every millisecond.  Live, pause
 * and sigsuspend wait as a sleep in simulated time until a timer fired here
 * (Pin keeps the guest's signal mask itself: a probe would not see it);
 * without a simulator they run, counting as waits, and the internal thread's
 * signal ends them. */

typedef Sift::GuestTimers<PinLock> Timers;
Timers *timers = NULL;
UINT64 *timer_check_at = NULL;
volatile UINT64 timers_fired = 0;   /* how many fired: a waiter sees one came */
const UINT64 TIMER_CHECK = 65536, TIMER_CHECK_MIN = 256;
volatile UINT64 timer_sim_ns = 0;   /* live: the simulator's time at the last look */

/* Live, the instructions until the next look (see the QEMU plugin's
   timer_check_interval) */
UINT64 timer_check_interval()
{
   const UINT64 next = timers->next_time(), now = timer_sim_ns;
   if (next == Sift::NO_DEADLINE)
      return TIMER_CHECK;
   const UINT64 left = next > now ? (next - now) / 8 : 0;
   return left < TIMER_CHECK_MIN ? TIMER_CHECK_MIN : left > TIMER_CHECK ? TIMER_CHECK : left;
}

UINT64 timer_thread_cpu(unsigned slot, void *)
{
   return Sift::one_ipc_ns(thread_data[slot].icount_total);
}

void timer_fire(const Sift::GuestTimer &t)
{
   if (t.notify == Sift::NOTIFY_NONE)
      return;
   if (t.notify == Sift::NOTIFY_TIMERFD)
   {
      const struct itimerspec now = { { 0, 0 }, { 0, 1 } };
      const UINT64 a[6] = { (UINT64)t.fd, 0, (UINT64)&now, 0, 0, 0 };
      host_syscall(SYS_timerfd_settime, a);
      return;
   }
   const UINT64 pid = (UINT64)getpid();
   __sync_fetch_and_add(&timers_fired, 1);
   {
      /* a thread in a sigtimedwait for it takes it at once (SigWait) */
      siginfo_t si;
      memset(&si, 0, sizeof(si));
      si.si_signo = t.signo;
      if (t.notify == Sift::NOTIFY_ITIMER)
         si.si_code = SI_KERNEL;
      else
      {
         si.si_code = SI_TIMER;
         si.si_timerid = t.id - Timers::FIRST_POSIX;
         si.si_overrun = (int)t.overrun;
         si.si_value.sival_ptr = (void *)t.value;
      }
      if (t.notify != Sift::NOTIFY_THREAD && sigwait_give(si))
         return;
   }
   /* an itimer's by kill(): see the QEMU plugin's timer_fire */
   if (t.notify == Sift::NOTIFY_ITIMER)
   {
      const UINT64 a[6] = { pid, (UINT64)t.signo, 0, 0, 0, 0 };
      host_syscall(SYS_kill, a);
      return;
   }
   siginfo_t si;
   memset(&si, 0, sizeof(si));
   si.si_signo = t.signo;
   {
      si.si_code = SI_TIMER;
      si.si_timerid = t.id - Timers::FIRST_POSIX;
      si.si_overrun = (int)t.overrun;
      si.si_value.sival_ptr = (void *)t.value;
   }
   if (t.notify == Sift::NOTIFY_THREAD)
   {
      const UINT64 a[6] = { pid, (UINT64)t.tid, (UINT64)t.signo, (UINT64)&si, 0, 0 };
      host_syscall(SYS_rt_tgsigqueueinfo, a);
   }
   else
   {
      const UINT64 a[6] = { pid, (UINT64)t.signo, (UINT64)&si, 0, 0, 0 };
      host_syscall(SYS_rt_sigqueueinfo, a);
   }
}

/* Fire what is due.  threadid: the thread looking, or -1 (the internal
   thread, without a simulator).  Returns how many fired. */
unsigned timers_check(INT32 threadid)
{
   if (!timers || !timers->armed())
      return 0;
   UINT64 time = Sift::NO_DEADLINE;
   const bool on_line = threadid >= 0 && live(threadid);
   if (on_line)
   {
      UINT64 t;
      if (sim_time(threadid, &t))
      {
         time = t;
         timer_sim_ns = t;
      }
   }
   else
   {
      static volatile UINT64 idle_epoch = ~0ull;
      UINT64 t, epoch;
      const int r = clocks->process_time(&t, &epoch);
      if (r == 1)
         time = t;
      else if (r == 2 && __sync_lock_test_and_set(&idle_epoch, epoch) == epoch)
         time = t;
   }
   Sift::GuestTimer due[16];
   /* the CPU-time timers: the process's from the simulator, live (see the
      QEMU plugin's timers_check) */
   const UINT64 pcpu = on_line && timers->cpu_armed() ? process_cpu_now(threadid) : clocks->process_cpu();
   const unsigned n = timers->due(time, pcpu, timer_thread_cpu, NULL, due, 16);
   for (unsigned i = 0; i < n; i++)
   {
      if (!on_line && due[i].clock == Sift::TIMER_TIME)
         clocks->woke_at(due[i].expiry);
      timer_fire(due[i]);
   }
   if (!on_line)
      clocks->set_event(timers->next_time());
   return n;
}

VOID timers_helper(VOID *)
{
   struct timespec ms = { 0, 1000000 };
   const UINT64 a[6] = { (UINT64)&ms, 0, 0, 0, 0, 0 };
   for (;;)
   {
      host_syscall(SYS_nanosleep, a);   /* the recorder's own millisecond */
      timers_check(-1);
   }
}

void timers_helper_start()
{
   static volatile int started = 0;
   if (KnobEmulateSyscalls.Value() || __sync_lock_test_and_set(&started, 1))
      return;
   PIN_SpawnInternalThread(timers_helper, NULL, 0, NULL);
}

UINT64 timer_now(THREADID threadid, Sift::TimerClock clock, unsigned cpu_slot)
{
   UINT64 tcpu;
   if (clock == Sift::TIMER_PROCESS_CPU)
      return process_cpu_now(threadid);
   if (clock == Sift::TIMER_THREAD_CPU)
      return cpu_slot == threadid && sim_cpu_times(threadid, &tcpu, NULL) ? tcpu : timer_thread_cpu(cpu_slot, NULL);
   UINT64 t;
   if (live(threadid) && sim_time(threadid, &t))
      return t;
   return clocks->now(threadid);
}

bool timer_clock(THREADID threadid, INT64 clockid, Sift::TimerClock *clock, unsigned *slot)
{
   *slot = threadid;
   switch (clockid)
   {
      case 0: case 1: case 4: case 5: case 6: case 7: case 8: case 9: case 11:
         *clock = Sift::TIMER_TIME;
         return true;
      case 2:
         *clock = Sift::TIMER_PROCESS_CPU;
         return true;
      case 3:
         *clock = Sift::TIMER_THREAD_CPU;
         return true;
      default:
         if (clockid < 0)
         {
            *clock = (clockid & 4) ? Sift::TIMER_THREAD_CPU : Sift::TIMER_PROCESS_CPU;
            return true;
         }
         return false;
   }
}

bool read_spec(ADDRINT addr, UINT64 *interval, UINT64 *value, bool timeval)
{
   INT64 w[4];
   if (!read_mem(addr, w, sizeof(w)))
      return false;
   const INT64 frac = timeval ? 1000000 : 1000000000, mul = timeval ? 1000 : 1;
   for (int i = 0; i < 4; i += 2)
      if (w[i] < 0 || w[i + 1] < 0 || w[i + 1] >= frac)
         return false;
   *interval = (UINT64)w[0] * Sift::NS_PER_S + (UINT64)w[1] * mul;
   *value = (UINT64)w[2] * Sift::NS_PER_S + (UINT64)w[3] * mul;
   return true;
}

bool write_spec(ADDRINT addr, UINT64 interval, UINT64 value, bool timeval)
{
   const UINT64 div = timeval ? 1000 : 1;
   const UINT64 w[4] = { interval / Sift::NS_PER_S, interval % Sift::NS_PER_S / div,
                         value / Sift::NS_PER_S, value % Sift::NS_PER_S / div };
   return write_mem(addr, w, sizeof(w));
}

long timer_arm(THREADID threadid, int id, ADDRINT new_addr, ADDRINT old_addr, bool absolute, bool timeval)
{
   const Sift::GuestTimer t = timers->get(id);
   UINT64 interval = 0, value = 0;
   if (new_addr && !read_spec(new_addr, &interval, &value, timeval))
      return -EINVAL;
   const UINT64 now = timer_now(threadid, t.clock, t.cpu_slot);
   UINT64 expiry = 0;
   if (value)
   {
      if (!absolute)
         expiry = now + value;
      else if (t.clock == Sift::TIMER_TIME)
      {
         const UINT64 zero = guestTimeZeroNs();
         expiry = value > zero ? value - zero : 1;
      }
      else
         expiry = value;
      if (!expiry)
         expiry = 1;
   }
   UINT64 old_left, old_interval;
   timers->settime(id, now, expiry, interval, &old_left, &old_interval);
   if (old_addr && !write_spec(old_addr, old_interval, old_left, timeval))
      return -EFAULT;
   if (expiry)
      timers_helper_start();
   if (!live(threadid))
      clocks->set_event(timers->next_time());
   timers_check(threadid);
   return 0;
}

long timer_get(THREADID threadid, int id, ADDRINT addr, bool timeval)
{
   const Sift::GuestTimer t = timers->get(id);
   UINT64 left, interval;
   timers->gettime(id, timer_now(threadid, t.clock, t.cpu_slot), &left, &interval);
   return write_spec(addr, interval, left, timeval) ? 0 : -EFAULT;
}

/* Live pause and sigsuspend: sleeps in simulated time until a timer fired */
long suspend_live(THREADID threadid, int x86, const syscall_args_t &args)
{
   const UINT64 fired = timers_fired;
   UINT64 sleep = LIVE_SLEEP_MIN;
   struct timespec nap;
   for (;;)
   {
      timers_check(threadid);
      if (timers_fired != fired)
      {
         mark_interrupted(threadid, x86, args);
         return -EINTR;
      }
      put_time(&nap, sleep);
      syscall_args_t sa = { (UINT64)&nap, 0, 0, 0, 0, 0 };
      thread_data[threadid].output->Syscall(SYS_nanosleep, (char *)sa, sizeof(sa));
      sleep = sleep * 2 < LIVE_SLEEP_MAX ? sleep * 2 : LIVE_SLEEP_MAX;
   }
}

/* Live, a blocking read of a timerfd kept here: see the QEMU plugin's
   timerfd_wait_live */
long timerfd_wait_live(THREADID threadid, int fd)
{
   const UINT64 g[6] = { (UINT64)fd, 3 /* F_GETFL */, 0, 0, 0, 0 };
   const long fl = host_syscall(SYS_fcntl, g);
   if (fl < 0 || (fl & O_NONBLOCK))
      return 0;
   UINT64 sleep = LIVE_SLEEP_MIN;
   struct timespec zero = { 0, 0 }, nap;
   for (;;)
   {
      timers_check(threadid);
      struct { INT32 fd; INT16 events, revents; } pfd = { fd, 1, 0 };
      const UINT64 a[6] = { (UINT64)&pfd, 1, (UINT64)&zero, 0, 8, 0 };
      const long r = host_syscall(SYS_ppoll, a);
      if (r != 0)
         return r == -EINTR ? r : 0;
      put_time(&nap, sleep);
      syscall_args_t sa = { (UINT64)&nap, 0, 0, 0, 0, 0 };
      thread_data[threadid].output->Syscall(SYS_nanosleep, (char *)sa, sizeof(sa));
      sleep = sleep * 2 < LIVE_SLEEP_MAX ? sleep * 2 : LIVE_SLEEP_MAX;
   }
}

/* The timer calls, times, getrusage, and live pause and sigsuspend: true
   when answered here */
bool timer_call(THREADID threadid, int x86, const syscall_args_t &args, ADDRINT *ret)
{
   long r;
   switch (x86)
   {
      case 37:   /* alarm */
      {
         const UINT64 now = timer_now(threadid, Sift::TIMER_TIME, 0);
         UINT64 left;
         const UINT64 secs = (UINT32)args[0];
         timers->settime(0, now, secs ? now + secs * Sift::NS_PER_S : 0, 0, &left, NULL);
         if (secs)
            timers_helper_start();
         if (!live(threadid))
            clocks->set_event(timers->next_time());
         UINT64 s = left / Sift::NS_PER_S + (left % Sift::NS_PER_S >= 500000000 ? 1 : 0);
         if (left && !s)
            s = 1;
         *ret = (ADDRINT)s;
         return true;
      }
      case 38:
         r = args[0] > 2 ? -EINVAL : timer_arm(threadid, (int)args[0], args[1], args[2], false, true);
         break;
      case 36:
         r = args[0] > 2 ? -EINVAL : timer_get(threadid, (int)args[0], args[1], true);
         break;
      case 222:  /* timer_create(clockid, sevp, timerid) */
      {
         Sift::GuestTimer spec;
         if (!timer_clock(threadid, (INT64)(INT32)args[0], &spec.clock, &spec.cpu_slot))
         {
            r = -EINVAL;
            break;
         }
         spec.notify = Sift::NOTIFY_SIGNAL;
         spec.signo = SIGALRM;
         bool value_is_id = true;
         if (args[1])
         {
            struct { UINT64 value; INT32 signo, notify, tid; } ev;
            if (!read_mem(args[1], &ev, sizeof(ev)))
            {
               r = -EFAULT;
               break;
            }
            value_is_id = false;
            spec.value = ev.value;
            spec.signo = ev.signo;
            spec.tid = ev.tid;
            if (ev.notify == 0)
               spec.notify = Sift::NOTIFY_SIGNAL;
            else if (ev.notify == 1)
               spec.notify = Sift::NOTIFY_NONE;
            else if (ev.notify == 4)
               spec.notify = Sift::NOTIFY_THREAD;
            else
            {
               r = -EINVAL;
               break;
            }
            if (spec.notify != Sift::NOTIFY_NONE && (spec.signo < 1 || spec.signo > 64))
            {
               r = -EINVAL;
               break;
            }
         }
         const int id = timers->create(spec);
         if (id < 0)
         {
            r = -EAGAIN;
            break;
         }
         if (value_is_id)
            timers->set_value(id, (UINT64)(id - Timers::FIRST_POSIX));
         const INT32 guest_id = id - Timers::FIRST_POSIX;
         r = write_mem(args[2], &guest_id, sizeof(guest_id)) ? 0 : -EFAULT;
         if (r)
            timers->remove(id);
         break;
      }
      case 223: case 224: case 225: case 226:
      {
         const int id = (int)(INT32)args[0] + Timers::FIRST_POSIX;
         if ((INT32)args[0] < 0 || !timers->valid(id) || timers->get(id).notify == Sift::NOTIFY_TIMERFD)
            r = -EINVAL;
         else if (x86 == 223)
            r = timer_arm(threadid, id, args[2], args[3], (args[1] & 1) != 0, false);
         else if (x86 == 224)
            r = timer_get(threadid, id, args[1], false);
         else if (x86 == 225)
         {
            const UINT64 o = timers->overrun(id);
            r = (long)(o < 0x7fffffff ? o : 0x7fffffff);
         }
         else
         {
            timers->remove(id);
            r = 0;
         }
         break;
      }
      case 286: case 287:  /* timerfd_settime, timerfd_gettime */
      {
         int id = timers->find((int)args[0]);
         if (id < 0)
            id = timers->timerfd((int)args[0], Sift::TIMER_TIME);
         if (id < 0)
            r = -EAGAIN;
         else if (x86 == 287)
            r = timer_get(threadid, id, args[1], false);
         else
         {
            const struct itimerspec off = { { 0, 0 }, { 0, 0 } };
            const UINT64 a[6] = { args[0], 0, (UINT64)&off, 0, 0, 0 };
            r = host_syscall(SYS_timerfd_settime, a);
            if (r != 0)
               timers->forget_fd((int)args[0]);
            else
               r = timer_arm(threadid, id, args[2], args[3], (args[1] & 1) != 0, false);
         }
         break;
      }
      case 283:  /* timerfd_create(clockid, flags): made here, its timer on its clock */
      {
         Sift::TimerClock clock;
         unsigned slot;
         if (!timer_clock(threadid, (INT64)(INT32)args[0], &clock, &slot))
            return false;              /* the kernel's EINVAL */
         r = host_syscall(SYS_timerfd_create, args);
         if (r >= 0)
         {
            timers->forget_fd((int)r);
            timers->timerfd((int)r, clock);
         }
         break;
      }
      case 3:    /* close: a timerfd's timer goes with it */
         timers->forget_fd((int)args[0]);
         return false;
      case 0: case 19:   /* read, readv: live, of a timerfd, wait for it first */
         if (live(threadid) && timers->find((int)args[0]) >= 0)
         {
            r = timerfd_wait_live(threadid, (int)args[0]);
            if (r)
               break;
         }
         return false;
      case 34: case 130:   /* pause, rt_sigsuspend */
         if (!live(threadid))
            return false;
         r = suspend_live(threadid, x86, args);
         break;
      case 100:  /* times(buf) */
      {
         const UINT64 tms[4] = { process_cpu_now(threadid) / 10000000, 0, 0, 0 };
         if (args[0] && !write_mem(args[0], tms, sizeof(tms)))
            r = -EFAULT;
         else
            r = (long)((guestTimeZeroNs() + timer_now(threadid, Sift::TIMER_TIME, 0)) / 10000000);
         break;
      }
      case 98:   /* getrusage(who, usage): the kernel's, but the CPU times */
      {
         r = host_syscall(98, args);
         if (r == 0 && args[1] && ((INT64)args[0] == 0 || (INT64)args[0] == 1))
         {
            UINT64 tcpu;
            const UINT64 user = (INT64)args[0] == 1 ? (sim_cpu_times(threadid, &tcpu, NULL) ? tcpu : timer_thread_cpu(threadid, NULL))
                                                   : process_cpu_now(threadid);
            const UINT64 tv[4] = { user / Sift::NS_PER_S, user % Sift::NS_PER_S / 1000, 0, 0 };
            if (!write_mem(args[1], tv, sizeof(tv)))
               r = -EFAULT;
         }
         break;
      }
      default:
         return false;
   }
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
   timers = new Timers();
   timer_check_at = new UINT64[max_num_threads]();
   interrupted = new Interrupted[max_num_threads]();
   sigwaits = new SigWait[max_num_threads]();
   PIN_InitLock(&sigwait_lock);
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
   if (restarted_after_signal(threadid, num, args))
   {
      *ret = (ADDRINT)-EINTR;
      return true;
   }
   timers_check(threadid);
   if (timer_call(threadid, x86, args, ret))
      return true;
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

void timedFlow(THREADID threadid)
{
   if (!TIMED_CALLS || !timers || !timers->armed() || !live(threadid))
      return;
   const UINT64 n = thread_data[threadid].icount_total;
   if (n - timer_check_at[threadid] < timer_check_interval())
      return;
   timer_check_at[threadid] = n;
   timers_check(threadid);
}

void timedSyscallReturned(THREADID threadid, ADDRINT ret)
{
   if (!waiting || !waiting[threadid])
      return;
   waiting[threadid] = false;
   clocks->wait_end(threadid, (long)ret == -EINTR ? Clocks::INTERRUPTED : Clocks::WOKEN);
}
