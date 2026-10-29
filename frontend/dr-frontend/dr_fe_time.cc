#include "dr_api.h"
#include "dr_fe_time.h"
#include "guest_time.h"
#include "sift_writer.h"

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <vector>

#ifndef CLONE_THREAD
# define CLONE_THREAD 0x00010000
#endif
#ifndef CLONE_CHILD_CLEARTID
# define CLONE_CHILD_CLEARTID 0x00200000
#endif

/* The guest's time under DynamoRIO (sift/guest_time.h): no time the guest
 * gives reaches the host's kernel, and the guest reads simulated time,
 * never the host's.  The SIFT recorder's emulation.cc, syscall_modeling.cc
 * and timed_calls.cc, for DynamoRIO.
 *
 * The clocks -- clock_gettime, gettimeofday and time, the vDSO's routines
 * (replaced, dr_frontend.cc) and the system calls (time_call), and rdtsc --
 * are the simulator's, live (a simulator answering: -e, the thread's
 * channel open), and otherwise the one-IPC model's (Sift::GuestClocks:
 * a thread's instructions at 1 GHz from its parent's time at its start,
 * plus what it waited), from the time zero (-time_start, osemu/time_start).
 *
 * The guest's process is this one, so a timed wait the simulator does not
 * model is run here, in the pre-syscall event, which then skips the
 * guest's call -- with timeouts of the front end's own, never the guest's:
 *
 * - live, as probes that do not wait (a zero timeout), between which the
 *   simulator lets the thread sleep in simulated time (a nanosleep it
 *   models, of 1 us doubling to 1 ms), until the guest's deadline in
 *   simulated time;
 * - where the simulator does not answer (a trace written to a file), in
 *   slices of SLICE_NS of host time that only let the front end look again,
 *   until the one-IPC model says the deadline has passed.
 *
 * A signal that arrives during such a wait ends it with EINTR: DynamoRIO
 * takes it in its handler (the application's signal mask is DynamoRIO's to
 * keep; the kernel's leaves the signal open), the front end's own call
 * returns EINTR, and DynamoRIO delivers the signal once the skipped call
 * has returned -- after the call, not in place of it, so it does not start
 * over as under Pin.  The front end's calls are made with the syscall
 * instruction: libc's would be the private loader's.
 *
 * x86-64 numbers and calling convention; another ISA is refused (init). */

#if defined(X86_64)
# define DR_TIME 1
#else
# define DR_TIME 0
#endif

namespace
{

struct DrLock
{
   void *m;
   DrLock() : m(dr_mutex_create()) {}
   ~DrLock() { dr_mutex_destroy(m); }
   void lock() { dr_mutex_lock(m); }
   void unlock() { dr_mutex_unlock(m); }
};

thread_data_t *thread_data = NULL;
bool emulate = false;
uint64_t time_start = Sift::TIME_START_DEFAULT;
const unsigned MAX_THREADS = MAX_NUM_THREADS;

/* Each thread's instructions (written by itself only) */
uint64_t insns[MAX_NUM_THREADS];

uint64_t clock_instructions(unsigned slot, void *)
{
   return __atomic_load_n(&insns[slot], __ATOMIC_RELAXED);
}

typedef Sift::GuestClocks<DrLock> Clocks;
Clocks *clocks = NULL;

bool live(int threadid)
{
   return threadid >= 0 && emulate && thread_data[threadid].output && thread_data[threadid].output->IsOpen();
}

/* x86-64 numbers of the other calls made or recognised here */
enum
{
   X_read = 0, X_close = 3, X_gettid = 186, X_tgkill = 234, X_readv = 19, X_getpid = 39, X_clone = 56, X_kill = 62, X_fcntl = 72,
   X_gettimeofday = 96, X_getrusage = 98, X_times = 100, X_rt_sigqueueinfo = 129, X_rt_sigsuspend = 130,
   X_pause = 34, X_getitimer = 36, X_setitimer = 38, X_time = 201, X_timer_create = 222,
   X_timer_settime = 223, X_timer_gettime = 224, X_timer_getoverrun = 225, X_timer_delete = 226,
   X_clock_gettime = 228, X_timerfd_create = 283, X_timerfd_settime = 286, X_timerfd_gettime = 287,
   X_rt_tgsigqueueinfo = 297, X_clone3 = 435,
};

bool read_mem(uint64_t addr, void *dst, size_t size)
{
   size_t got = 0;
   return dr_safe_read((const void *)addr, size, dst, &got) && got == size;
}

bool write_mem(uint64_t addr, const void *src, size_t size)
{
   size_t put = 0;
   return dr_safe_write((void *)addr, size, src, &put) && put == size;
}

/* ---------------------------------------------------------------- clocks */

/* The one-IPC model's reading of a clock */
uint64_t one_ipc_clock(threadid_t threadid, int64_t clk)
{
   const bool thread_cpu = clk == CLOCK_THREAD_CPUTIME_ID || (clk < 0 && (clk & 4));
   const bool process_cpu = !thread_cpu && Sift::cputime_clock((int)clk);
   return thread_cpu ? clocks->thread_cpu(threadid) : process_cpu ? clocks->process_cpu()
        : time_start * Sift::NS_PER_S + clocks->now(threadid);
}

/* The clocks' system calls where the simulator does not answer (a trace
   written to a file): the one-IPC model's time; time(2), which the
   simulator does not model, from its time, live */
bool time_call(threadid_t threadid, int x86, const uint64_t args[6], uint64_t *ret)
{
   if (x86 != X_clock_gettime && x86 != X_gettimeofday && x86 != X_time)
      return false;
   const bool tod = x86 == X_gettimeofday, secs = x86 == X_time;
   uint64_t ns;
   if (live(threadid))
   {
      if (!secs)
         return false;              /* the simulator's (doSyscall) */
      Sift::EmuRequest req;
      Sift::EmuReply res;
      if (!thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res))
         return false;
      ns = res.gettime.time_ns;
   }
   else
   {
      const int64_t clk = tod || secs ? 0 : (int64_t)(int32_t)args[0];
      if (!tod && !secs && !drtime::clock_ns(threadid, clk, &ns))
      {
         *ret = (uint64_t)-EINVAL;
         return true;
      }
      if (tod || secs)
         ns = one_ipc_clock(threadid, 0);
   }
   const uint64_t words[2] = { ns / Sift::NS_PER_S, tod ? ns / 1000 % 1000000 : ns % Sift::NS_PER_S };
   const uint64_t out = tod || secs ? args[0] : args[1];
   if (out && !write_mem(out, words, secs ? sizeof(words[0]) : sizeof(words)))
   {
      *ret = (uint64_t)-EFAULT;
      return true;
   }
   if (tod && args[1])
   {
      const int32_t tz[2] = { 0, 0 };
      write_mem(args[1], tz, sizeof(tz));
   }
   *ret = secs ? words[0] : 0;
   return true;
}

/* ---------------------------------------------------------------- threads
 *
 * A clone's child starts at its parent's time (GuestClocks::start); its
 * clear-tid word, which its exit wakes, is its joiner's.  The child is
 * paired with its clone by the stack pointer it starts with (as the
 * generic layer pairs it with its join word). */

struct Clone { uint64_t stack, tid_word; int parent; };
std::vector<Clone> *clones = NULL;
DrLock *clone_lock = NULL;
uint64_t tid_words[MAX_NUM_THREADS];

void note_clone(threadid_t threadid, int x86, const uint64_t args[6])
{
   Clone c = { 0, 0, (int)threadid };
   if (x86 == X_clone)
   {
      if (!(args[0] & CLONE_THREAD))
         return;
      c.stack = args[1];
      c.tid_word = (args[0] & CLONE_CHILD_CLEARTID) ? args[3] : 0;
   }
   else
   {
      uint64_t cl[8];   /* flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls */
      if (!args[0] || !read_mem(args[0], cl, sizeof(cl)) || !(cl[0] & CLONE_THREAD))
         return;
      c.stack = cl[5] + cl[6];
      c.tid_word = (cl[0] & CLONE_CHILD_CLEARTID) ? cl[2] : 0;
   }
   clone_lock->lock();
   if (clones->size() > 1024)
      clones->erase(clones->begin());   /* clones that failed: never taken */
   clones->push_back(c);
   clone_lock->unlock();
}

/* ---------------------------------------------------------------- timed waits */

/* the threads in a call that can block, where the simulator does not answer
   (each written by its own thread only) */
bool waiting[MAX_NUM_THREADS];

const uint64_t SLICE_NS = 1000000;
const uint64_t LIVE_SLEEP_MIN = 1000, LIVE_SLEEP_MAX = 1000000;

/* The system call itself, from the front end: DynamoRIO does not see it */
long host_syscall(long num, const uint64_t a[6])
{
#if DR_TIME
   long r;
   register uint64_t r10 __asm__("r10") = a[3];
   register uint64_t r8 __asm__("r8") = a[4];
   register uint64_t r9 __asm__("r9") = a[5];
   __asm__ volatile("syscall" : "=a"(r) : "a"(num), "D"(a[0]), "S"(a[1]), "d"(a[2]), "r"(r10), "r"(r8), "r"(r9)
                    : "rcx", "r11", "memory");
   return r;
#else
   (void)num; (void)a;
   return -ENOSYS;
#endif
}

void put_time(struct timespec *ts, uint64_t ns)
{
   ts->tv_sec = ns / Sift::NS_PER_S;
   ts->tv_nsec = ns % Sift::NS_PER_S;
}

/* The simulator's time, without the clocks' zero */
bool sim_time(threadid_t threadid, uint64_t *ns)
{
   Sift::EmuRequest req;
   Sift::EmuReply res;
   if (!thread_data[threadid].output->Emulate(Sift::EmuTypeCounters, req, res))
      return false;
   *ns = res.counters.time_ns;
   return true;
}

/* A nanosleep the simulator models: the thread sleeps in simulated time */
void sim_sleep(threadid_t threadid, uint64_t ns)
{
   struct timespec nap;
   put_time(&nap, ns);
   syscall_args_t sa = { (uint64_t)&nap, 0, 0, 0, 0, 0 };
   thread_data[threadid].output->Syscall(Sift::X86_nanosleep, (char *)sa, sizeof(sa));
}

/* The time a timed call ends at (see the QEMU plugin's call_deadline) */
bool call_deadline(const Sift::TimedCall &tc, const uint64_t args[6], uint64_t now, uint64_t cpu_now,
                   uint64_t *deadline, long *err)
{
   uint64_t v;
   if (tc.form == Sift::TO_MS)
      v = (uint64_t)(int64_t)(int32_t)args[tc.arg] * 1000000ull;
   else
   {
      int64_t w[2];
      if (!read_mem(args[tc.arg], w, sizeof(w)))
      {
         *err = -EFAULT;
         return false;
      }
      const int64_t frac_max = tc.form == Sift::TO_TIMEVAL ? 1000000 : 1000000000;
      if (w[0] < 0 || w[1] < 0 || w[1] >= frac_max)
      {
         *err = -EINVAL;
         return false;
      }
      const uint64_t sec = (uint64_t)w[0] > 9000000000ull ? 9000000000ull : (uint64_t)w[0];
      v = sec * Sift::NS_PER_S + (uint64_t)w[1] * (tc.form == Sift::TO_TIMEVAL ? 1000 : 1);
   }
   if (!tc.absolute)
      *deadline = now + v;
   else if (Sift::cputime_clock(tc.clock))
      *deadline = now + (v > cpu_now ? v - cpu_now : 0);
   else
   {
      const uint64_t zero = time_start * Sift::NS_PER_S;
      *deadline = v > zero ? v - zero : 0;
   }
   return true;
}

/* The guest's arguments with the timeout replaced: ns of waiting (0: a
   probe), in *own (a timespec or timeval of the front end's) */
void own_timeout(const Sift::TimedCall &tc, uint64_t a[6], uint64_t ns, struct timespec *own)
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
         a[tc.arg] = (uint64_t)tv;
         return;
      }
      default:
         if (tc.absolute)
         {
            uint64_t at = 0;   /* the epoch: past */
            if (ns)
            {
               struct timespec t;
               const uint64_t g[6] = { (uint64_t)(tc.clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC), (uint64_t)&t, 0, 0, 0, 0 };
               host_syscall(X_clock_gettime, g);   /* the host's clock, for the host's kernel */
               at = (uint64_t)t.tv_sec * Sift::NS_PER_S + (uint64_t)t.tv_nsec + ns;
            }
            put_time(own, at);
         }
         else
            put_time(own, ns);
         a[tc.arg] = (uint64_t)own;
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
   std::vector<uint8_t> set[3];
   bool have[3];
   FdSets() { have[0] = have[1] = have[2] = false; }
   bool save(const uint64_t args[6])
   {
      const uint64_t n = args[0];
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
   void restore(const uint64_t args[6]) const
   {
      for (int i = 0; i < 3; i++)
         if (have[i])
            write_mem(args[1 + i], &set[i][0], set[i].size());
   }
};

/* the time left, as the kernel leaves it (see the QEMU plugin's time_left) */
void time_left(const Sift::TimedCall &tc, const uint64_t args[6], int x86, uint64_t deadline, uint64_t now, long ret)
{
   const uint64_t left = deadline != Sift::NO_DEADLINE && deadline > now ? deadline - now : 0;
   if ((x86 == Sift::X86_select || x86 == Sift::X86_pselect6 || x86 == Sift::X86_ppoll)
       && args[tc.arg] && deadline != Sift::NO_DEADLINE)
   {
      const uint64_t w[2] = { left / Sift::NS_PER_S, x86 == Sift::X86_select ? left % Sift::NS_PER_S / 1000 : left % Sift::NS_PER_S };
      write_mem(args[tc.arg], w, sizeof(w));
   }
   if (tc.cls == Sift::TC_SLEEP && ret == -EINTR && !tc.absolute && tc.rem_arg >= 0 && args[tc.rem_arg])
   {
      const uint64_t w[2] = { left / Sift::NS_PER_S, left % Sift::NS_PER_S };
      write_mem(args[tc.rem_arg], w, sizeof(w));
   }
}

bool recvmmsg_untimed(const uint64_t args[6], uint64_t *ret)
{
   uint64_t a[6];
   memcpy(a, args, sizeof(a));
   a[3] |= 0x10000;   /* MSG_WAITFORONE: see the QEMU plugin's recvmmsg_untimed */
   a[4] = 0;
   *ret = (uint64_t)host_syscall(Sift::X86_recvmmsg, a);
   return true;
}

unsigned timers_check(int threadid);   /* the timers, below */
uint64_t live_nap(uint64_t sleep);
extern volatile uint64_t timers_fired;

/* A thread in a sigtimedwait the front end runs, and the signals it waits
   for: a timer's signal among them is handed to it here, with its siginfo,
   rather than sent.  Sent, DynamoRIO would take it for the application (it
   keeps the application's signal mask itself, the kernel's leaves the
   signal open): the front end's own sigtimedwait would never see it. */
struct SigWait { volatile bool waiting, got; uint64_t set; siginfo_t info; };
SigWait sigwaits[MAX_NUM_THREADS];
DrLock *sigwait_lock = NULL;

void sigwait_begin(threadid_t threadid, const uint64_t args[6])
{
   uint64_t set = 0;
   read_mem(args[0], &set, args[3] < sizeof(set) ? args[3] : sizeof(set));
   sigwait_lock->lock();
   sigwaits[threadid].set = set;
   sigwaits[threadid].got = false;
   sigwaits[threadid].waiting = true;
   sigwait_lock->unlock();
}

/* The signal handed to the waiter (its number, its siginfo written), or 0 */
long sigwait_take(threadid_t threadid, const uint64_t args[6])
{
   if (!sigwaits[threadid].got)
      return 0;
   if (args[1])
      write_mem(args[1], &sigwaits[threadid].info, sizeof(siginfo_t));
   return sigwaits[threadid].info.si_signo;
}

void sigwait_end(threadid_t threadid)
{
   sigwait_lock->lock();
   sigwaits[threadid].waiting = false;
   sigwaits[threadid].got = false;
   sigwait_lock->unlock();
}

/* A timer's signal to a waiter whose set has it: true when handed over */
bool sigwait_give(const siginfo_t &info)
{
   bool given = false;
   sigwait_lock->lock();
   for (unsigned i = 0; i < MAX_THREADS && !given; i++)
      if (sigwaits[i].waiting && !sigwaits[i].got && (sigwaits[i].set >> (info.si_signo - 1)) & 1)
      {
         sigwaits[i].info = info;
         sigwaits[i].got = true;
         given = true;
      }
   sigwait_lock->unlock();
   return given;
}

/* The one-IPC model's timed wait */
bool timed_one_ipc(threadid_t threadid, int x86, const uint64_t args[6], const Sift::TimedCall &tc, uint64_t *ret)
{
   const uint64_t word = x86 == Sift::X86_futex ? args[0] : 0;
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
         *ret = (uint64_t)-EFAULT;
         return true;
      }
      clocks->wait_begin(threadid, Sift::NO_DEADLINE, word);
      waiting[threadid] = true;
      return false;
   }
   const uint64_t now = clocks->now(threadid);
   uint64_t deadline;
   long err = 0;
   if (!call_deadline(tc, args, now, clocks->process_cpu(), &deadline, &err))
   {
      *ret = (uint64_t)err;
      return true;
   }
   FdSets sets;
   if (tc.fdsets && !sets.save(args))
   {
      *ret = (uint64_t)-EFAULT;
      return true;
   }

   clocks->wait_begin(threadid, deadline, word);
   Clocks::End how = Clocks::WOKEN;
   long r = 0;
   bool idle_seen = false;
   uint64_t idle_epoch = 0;
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
         uint64_t epoch;
         const int m = clocks->may_expire(threadid, &epoch);
         expire = m == 1 || (m == 2 && idle_seen && epoch == idle_epoch);
         idle_seen = m == 2;
         idle_epoch = epoch;
      }
      uint64_t a[6];
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
         const uint64_t s[6] = { (uint64_t)&own, 0, 0, 0, 0, 0 };
         r = host_syscall(Sift::X86_nanosleep, s);
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
   *ret = (uint64_t)r;
   return true;
}

/* A live timed wait the simulator does not model */
bool timed_live(threadid_t threadid, int x86, const uint64_t args[6], const Sift::TimedCall &tc, uint64_t *ret)
{
   if (tc.cls == Sift::TC_SLEEP || x86 == Sift::X86_futex)
      return false;                 /* the simulator's (doSyscall) */
   if (x86 == Sift::X86_futex_waitv)
   {
      *ret = (uint64_t)-ENOSYS;     /* a futex wait outside the simulator's model of the futexes */
      return true;
   }
   if (x86 == Sift::X86_recvmmsg)
      return args[4] && recvmmsg_untimed(args, ret);
   uint64_t now;
   if (!sim_time(threadid, &now))
      return false;
   uint64_t deadline = Sift::NO_DEADLINE;
   long err = 0;
   if (!Sift::timeout_infinite(tc, args) && !call_deadline(tc, args, now, now, &deadline, &err))
   {
      *ret = (uint64_t)err;
      return true;
   }
   FdSets sets;
   if (tc.fdsets && !sets.save(args))
   {
      *ret = (uint64_t)-EFAULT;
      return true;
   }
   uint64_t sleep = LIVE_SLEEP_MIN;
   long r = 0;
   struct timespec own;
   if (x86 == Sift::X86_rt_sigtimedwait)
      sigwait_begin(threadid, args);
   for (;;)
   {
      uint64_t a[6];
      memcpy(a, args, sizeof(a));
      sets.restore(args);
      own_timeout(tc, a, 0, &own);
      r = host_syscall(x86, a);
      if (r != timeout_result(x86) || (deadline != Sift::NO_DEADLINE && now >= deadline))
         break;
      uint64_t s = sleep;
      if (deadline != Sift::NO_DEADLINE && deadline - now < s)
         s = deadline - now;
      sim_sleep(threadid, live_nap(s));
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
         /* a signal for the guest: the wait ends with EINTR, and DynamoRIO
            delivers the signal as the call returns */
         r = -EINTR;
         break;
      }
   }
   if (x86 == Sift::X86_rt_sigtimedwait)
      sigwait_end(threadid);
   time_left(tc, args, x86, deadline, now, r);
   *ret = (uint64_t)r;
   return true;
}


/* ---------------------------------------------------------------- timers
 *
 * alarm, setitimer, the POSIX timers and timerfd in the guest's time
 * (Sift::GuestTimers; see the recorder's and the QEMU plugin's timers):
 * kept here, fired when the guest's time passes their expiry -- the
 * simulator's, live, else the time every thread has reached in the one-IPC
 * model -- with calls that carry no time (rt_sigqueueinfo, as the kernel's
 * signal; the timerfd itself armed to expire at once).  They are looked at
 * in every system call, in the wait loops, live every so many instructions
 * (flow), and without a simulator by a client thread every millisecond.
 * Live, pause and sigsuspend wait as a sleep in simulated time until a
 * timer fired here; without a simulator they run, counting as waits, and
 * the client thread's signal ends them. */

typedef Sift::GuestTimers<DrLock> Timers;
Timers *timers = NULL;
uint64_t timer_check_at[MAX_NUM_THREADS];
volatile uint64_t timers_fired = 0;   /* how many fired: a waiter sees one came */
const uint64_t TIMER_CHECK = 65536, TIMER_CHECK_MIN = 256;
volatile uint64_t timer_sim_ns = 0;   /* live: the simulator's time at the last look */
volatile bool helper_stop = false;

/* Live, the instructions until the next look (see the QEMU plugin's
   timer_check_interval) */
uint64_t timer_check_interval()
{
   const uint64_t next = timers->next_time(), now = timer_sim_ns;
   if (next == Sift::NO_DEADLINE)
      return TIMER_CHECK;
   const uint64_t left = next > now ? (next - now) / 8 : 0;
   return left < TIMER_CHECK_MIN ? TIMER_CHECK_MIN : left > TIMER_CHECK ? TIMER_CHECK : left;
}

uint64_t timer_thread_cpu(unsigned slot, void *)
{
   return Sift::one_ipc_ns(clock_instructions(slot, NULL));
}

/* Each thread's kernel tid, while it runs (0: none) */
volatile int64_t host_tids[MAX_NUM_THREADS];

/* The application's process.  DynamoRIO's client thread is a process of its
   own (its getpid is its own): a kill from it to "its" process signalled
   itself, and the kernel refuses it a siginfo with a kernel's code (an
   itimer's SI_KERNEL) for another process. */
uint64_t app_pid = 0;

/* A thread to signal for the process, from the client thread: the first
   that runs (its slot in *slot) */
int64_t signal_target(unsigned *slot)
{
   for (unsigned i = 0; i < MAX_THREADS; i++)
      if (host_tids[i])
      {
         *slot = i;
         return host_tids[i];
      }
   return 0;
}

/* A signal the client thread sent a thread (signal_target) reaches
   DynamoRIO's handler, which delivers it as the thread leaves the code
   cache -- and a thread computing in linked blocks, or in clean calls (the
   vDSO's clocks), does not leave: a trace of timers.c, itimer-spin, read a
   whole second of the guest's time without its SIGALRM.  So the thread is
   kicked out at its next blocks (flow) until DynamoRIO delivers a signal to
   it (signal_delivered), at most KICKS times (a signal the application
   blocks stays pending). */
const int KICKS = 4096;
volatile int kicks[MAX_NUM_THREADS];

/* Fire a timer: from the thread that looked (threadid), or from the client
   thread (-1) */
void timer_fire(const Sift::GuestTimer &t, int threadid)
{
   if (t.notify == Sift::NOTIFY_NONE)
      return;
   if (t.notify == Sift::NOTIFY_TIMERFD)
   {
      const struct itimerspec now = { { 0, 0 }, { 0, 1 } };
      const uint64_t a[6] = { (uint64_t)t.fd, 0, (uint64_t)&now, 0, 0, 0 };
      host_syscall(X_timerfd_settime, a);
      return;
   }
   const uint64_t pid = app_pid;
   __sync_fetch_and_add(&timers_fired, 1);
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
   /* a thread in a sigtimedwait for it takes it at once (SigWait) */
   if (t.notify != Sift::NOTIFY_THREAD && sigwait_give(si))
      return;
   unsigned target_slot = 0;
   const int64_t target = threadid < 0 && t.notify != Sift::NOTIFY_THREAD ? signal_target(&target_slot) : 0;
   if (target && t.notify == Sift::NOTIFY_ITIMER)
   {
      /* to one of the application's threads (SI_TKILL: SI_KERNEL is the
         kernel's own) */
      const uint64_t a[6] = { pid, (uint64_t)target, (uint64_t)t.signo, 0, 0, 0 };
      host_syscall(X_tgkill, a);
      kicks[target_slot] = KICKS;
   }
   else if (target && t.notify == Sift::NOTIFY_SIGNAL)
   {
      /* to one of the application's threads, with the POSIX timer's siginfo */
      const uint64_t a[6] = { pid, (uint64_t)target, (uint64_t)t.signo, (uint64_t)&si, 0, 0 };
      host_syscall(X_rt_tgsigqueueinfo, a);
      kicks[target_slot] = KICKS;
   }
   else if (t.notify == Sift::NOTIFY_ITIMER)
   {
      const uint64_t a[6] = { pid, (uint64_t)t.signo, 0, 0, 0, 0 };
      host_syscall(X_kill, a);
   }
   else if (t.notify == Sift::NOTIFY_THREAD)
   {
      const uint64_t a[6] = { pid, (uint64_t)t.tid, (uint64_t)t.signo, (uint64_t)&si, 0, 0 };
      host_syscall(X_rt_tgsigqueueinfo, a);
      if (threadid < 0)
         for (unsigned i = 0; i < MAX_THREADS; i++)
            if (host_tids[i] == t.tid)
               kicks[i] = KICKS;
   }
   else
   {
      const uint64_t a[6] = { pid, (uint64_t)t.signo, (uint64_t)&si, 0, 0, 0 };
      host_syscall(X_rt_sigqueueinfo, a);
   }
}

/* Fire what is due.  threadid: the thread looking, or -1 (the client
   thread, without a simulator).  Returns how many fired. */
unsigned timers_check(int threadid)
{
   if (!timers || !timers->armed())
      return 0;
   uint64_t time = Sift::NO_DEADLINE;
   const bool on_line = live(threadid);
   if (on_line)
   {
      uint64_t t;
      if (sim_time(threadid, &t))
      {
         time = t;
         timer_sim_ns = t;
      }
   }
   else
   {
      static volatile uint64_t idle_epoch = ~0ull;
      uint64_t t, epoch;
      const int r = clocks->process_time(&t, &epoch);
      if (r == 1)
         time = t;
      else if (r == 2 && __sync_lock_test_and_set(&idle_epoch, epoch) == epoch)
         time = t;
   }
   Sift::GuestTimer due[16];
   const unsigned n = timers->due(time, clocks->process_cpu(), timer_thread_cpu, NULL, due, 16);
   for (unsigned i = 0; i < n; i++)
   {
      if (!on_line && due[i].clock == Sift::TIMER_TIME)
         clocks->woke_at(due[i].expiry);
      timer_fire(due[i], threadid);
   }
   if (!on_line)
      clocks->set_event(timers->next_time());
   return n;
}

void timers_helper(void *)
{

   struct timespec ms = { 0, 1000000 };
   const uint64_t a[6] = { (uint64_t)&ms, 0, 0, 0, 0, 0 };
   while (!helper_stop)
   {
      host_syscall(Sift::X86_nanosleep, a);   /* the front end's own millisecond */
      if (!helper_stop)
         timers_check(-1);
   }
}

void timers_helper_start()
{
   static volatile int started = 0;
   if (emulate || __sync_lock_test_and_set(&started, 1))
      return;
   dr_create_client_thread(timers_helper, NULL);
}

uint64_t timer_now(threadid_t threadid, Sift::TimerClock clock, unsigned cpu_slot)
{
   if (clock == Sift::TIMER_PROCESS_CPU)
      return clocks->process_cpu();
   if (clock == Sift::TIMER_THREAD_CPU)
      return timer_thread_cpu(cpu_slot, NULL);
   uint64_t t;
   if (live(threadid) && sim_time(threadid, &t))
      return t;
   return clocks->now(threadid);
}

bool timer_clock(threadid_t threadid, int64_t clockid, Sift::TimerClock *clock, unsigned *slot)
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

bool read_spec(uint64_t addr, uint64_t *interval, uint64_t *value, bool timeval)
{
   int64_t w[4];
   if (!read_mem(addr, w, sizeof(w)))
      return false;
   const int64_t frac = timeval ? 1000000 : 1000000000, mul = timeval ? 1000 : 1;
   for (int i = 0; i < 4; i += 2)
      if (w[i] < 0 || w[i + 1] < 0 || w[i + 1] >= frac)
         return false;
   *interval = (uint64_t)w[0] * Sift::NS_PER_S + (uint64_t)w[1] * mul;
   *value = (uint64_t)w[2] * Sift::NS_PER_S + (uint64_t)w[3] * mul;
   return true;
}

bool write_spec(uint64_t addr, uint64_t interval, uint64_t value, bool timeval)
{
   const uint64_t div = timeval ? 1000 : 1;
   const uint64_t w[4] = { interval / Sift::NS_PER_S, interval % Sift::NS_PER_S / div,
                           value / Sift::NS_PER_S, value % Sift::NS_PER_S / div };
   return write_mem(addr, w, sizeof(w));
}

long timer_arm(threadid_t threadid, int id, uint64_t new_addr, uint64_t old_addr, bool absolute, bool timeval)
{
   const Sift::GuestTimer t = timers->get(id);
   uint64_t interval = 0, value = 0;
   if (new_addr && !read_spec(new_addr, &interval, &value, timeval))
      return -EINVAL;
   const uint64_t now = timer_now(threadid, t.clock, t.cpu_slot);
   uint64_t expiry = 0;
   if (value)
   {
      if (!absolute)
         expiry = now + value;
      else if (t.clock == Sift::TIMER_TIME)
      {
         const uint64_t zero = time_start * Sift::NS_PER_S;
         expiry = value > zero ? value - zero : 1;
      }
      else
         expiry = value;
      if (!expiry)
         expiry = 1;
   }
   uint64_t old_left, old_interval;
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

long timer_get(threadid_t threadid, int id, uint64_t addr, bool timeval)
{
   const Sift::GuestTimer t = timers->get(id);
   uint64_t left, interval;
   timers->gettime(id, timer_now(threadid, t.clock, t.cpu_slot), &left, &interval);
   return write_spec(addr, interval, left, timeval) ? 0 : -EFAULT;
}

/* A live sleep's length, up to the next timer's expiry on the guest's time
   (timer_sim_ns: the simulator's time at the last look): without the cap a
   thread in pause woke up to a whole nap, 1 ms at most, after its timer */
uint64_t live_nap(uint64_t sleep)
{
   const uint64_t next = timers->next_time(), now = timer_sim_ns;
   if (next != Sift::NO_DEADLINE && next > now && next - now < sleep)
      return next - now;
   return sleep;
}

/* Live pause and sigsuspend: sleeps in simulated time until a timer fired */
long suspend_live(threadid_t threadid)
{
   const uint64_t fired = timers_fired;
   uint64_t sleep = LIVE_SLEEP_MIN;
   for (;;)
   {
      timers_check(threadid);
      if (timers_fired != fired)
         return -EINTR;
      sim_sleep(threadid, live_nap(sleep));
      sleep = sleep * 2 < LIVE_SLEEP_MAX ? sleep * 2 : LIVE_SLEEP_MAX;
   }
}

/* Live, a blocking read of a timerfd kept here: see the QEMU plugin's
   timerfd_wait_live */
long timerfd_wait_live(threadid_t threadid, int fd)
{
   const uint64_t g[6] = { (uint64_t)fd, 3 /* F_GETFL */, 0, 0, 0, 0 };
   const long fl = host_syscall(X_fcntl, g);
   if (fl < 0 || (fl & 04000 /* O_NONBLOCK */))
      return 0;
   uint64_t sleep = LIVE_SLEEP_MIN;
   struct timespec zero = { 0, 0 };
   for (;;)
   {
      timers_check(threadid);
      struct { int32_t fd; int16_t events, revents; } pfd = { fd, 1, 0 };
      const uint64_t a[6] = { (uint64_t)&pfd, 1, (uint64_t)&zero, 0, 8, 0 };
      const long r = host_syscall(Sift::X86_ppoll, a);
      if (r != 0)
         return r == -EINTR ? r : 0;
      sim_sleep(threadid, live_nap(sleep));
      sleep = sleep * 2 < LIVE_SLEEP_MAX ? sleep * 2 : LIVE_SLEEP_MAX;
   }
}

/* The timer calls, times, getrusage, and live pause and sigsuspend: true
   when answered here */
bool timer_call(threadid_t threadid, int x86, const uint64_t args[6], uint64_t *ret)
{
   long r;
   switch (x86)
   {
      case Sift::X86_alarm:
      {
         const uint64_t now = timer_now(threadid, Sift::TIMER_TIME, 0);
         uint64_t left;
         const uint64_t secs = (uint32_t)args[0];
         timers->settime(0, now, secs ? now + secs * Sift::NS_PER_S : 0, 0, &left, NULL);
         if (secs)
            timers_helper_start();
         if (!live(threadid))
            clocks->set_event(timers->next_time());
         uint64_t s = left / Sift::NS_PER_S + (left % Sift::NS_PER_S >= 500000000 ? 1 : 0);
         if (left && !s)
            s = 1;
         *ret = s;
         return true;
      }
      case X_setitimer:
         r = args[0] > 2 ? -EINVAL : timer_arm(threadid, (int)args[0], args[1], args[2], false, true);
         break;
      case X_getitimer:
         r = args[0] > 2 ? -EINVAL : timer_get(threadid, (int)args[0], args[1], true);
         break;
      case X_timer_create:  /* timer_create(clockid, sevp, timerid) */
      {
         Sift::GuestTimer spec;
         if (!timer_clock(threadid, (int64_t)(int32_t)args[0], &spec.clock, &spec.cpu_slot))
         {
            r = -EINVAL;
            break;
         }
         spec.notify = Sift::NOTIFY_SIGNAL;
         spec.signo = SIGALRM;
         bool value_is_id = true;
         if (args[1])
         {
            struct { uint64_t value; int32_t signo, notify, tid; } ev;
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
            timers->set_value(id, (uint64_t)(id - Timers::FIRST_POSIX));
         const int32_t guest_id = id - Timers::FIRST_POSIX;
         r = write_mem(args[2], &guest_id, sizeof(guest_id)) ? 0 : -EFAULT;
         if (r)
            timers->remove(id);
         break;
      }
      case X_timer_settime: case X_timer_gettime: case X_timer_getoverrun: case X_timer_delete:
      {
         const int id = (int)(int32_t)args[0] + Timers::FIRST_POSIX;
         if ((int32_t)args[0] < 0 || !timers->valid(id) || timers->get(id).notify == Sift::NOTIFY_TIMERFD)
            r = -EINVAL;
         else if (x86 == X_timer_settime)
            r = timer_arm(threadid, id, args[2], args[3], (args[1] & 1) != 0, false);
         else if (x86 == X_timer_gettime)
            r = timer_get(threadid, id, args[1], false);
         else if (x86 == X_timer_getoverrun)
         {
            const uint64_t o = timers->overrun(id);
            r = (long)(o < 0x7fffffff ? o : 0x7fffffff);
         }
         else
         {
            timers->remove(id);
            r = 0;
         }
         break;
      }
      case X_timerfd_settime: case X_timerfd_gettime:
      {
         int id = timers->find((int)args[0]);
         if (id < 0)
            id = timers->timerfd((int)args[0], Sift::TIMER_TIME);
         if (id < 0)
            r = -EAGAIN;
         else if (x86 == X_timerfd_gettime)
            r = timer_get(threadid, id, args[1], false);
         else
         {
            /* the real timerfd disarmed (it checks the fd and the flags) */
            const struct itimerspec off = { { 0, 0 }, { 0, 0 } };
            const uint64_t a[6] = { args[0], 0, (uint64_t)&off, 0, 0, 0 };
            r = host_syscall(X_timerfd_settime, a);
            if (r != 0)
               timers->forget_fd((int)args[0]);
            else
               r = timer_arm(threadid, id, args[2], args[3], (args[1] & 1) != 0, false);
         }
         break;
      }
      case X_timerfd_create:  /* made here, its timer on its clock */
      {
         Sift::TimerClock clock;
         unsigned slot;
         if (!timer_clock(threadid, (int64_t)(int32_t)args[0], &clock, &slot))
            return false;              /* the kernel's EINVAL */
         r = host_syscall(X_timerfd_create, args);
         if (r >= 0)
         {
            timers->forget_fd((int)r);
            timers->timerfd((int)r, clock);
         }
         break;
      }
      case X_close:  /* a timerfd's timer goes with it */
         timers->forget_fd((int)args[0]);
         return false;
      case X_read: case X_readv:   /* live, of a timerfd, wait for it first */
         if (live(threadid) && timers->find((int)args[0]) >= 0)
         {
            r = timerfd_wait_live(threadid, (int)args[0]);
            if (r)
               break;
         }
         return false;
      case X_pause: case X_rt_sigsuspend:
         if (!live(threadid))
            return false;
         r = suspend_live(threadid);
         break;
      case X_times:  /* times(buf) */
      {
         const uint64_t tms[4] = { clocks->process_cpu() / 10000000, 0, 0, 0 };
         if (args[0] && !write_mem(args[0], tms, sizeof(tms)))
            r = -EFAULT;
         else
            r = (long)((time_start * Sift::NS_PER_S + timer_now(threadid, Sift::TIMER_TIME, 0)) / 10000000);
         break;
      }
      case X_getrusage:  /* getrusage(who, usage): the kernel's, but the CPU times */
      {
         r = host_syscall(X_getrusage, args);
         if (r == 0 && args[1] && ((int64_t)args[0] == 0 || (int64_t)args[0] == 1))
         {
            const uint64_t user = (int64_t)args[0] == 1 ? timer_thread_cpu(threadid, NULL) : clocks->process_cpu();
            const uint64_t tv[4] = { user / Sift::NS_PER_S, user % Sift::NS_PER_S / 1000, 0, 0 };
            if (!write_mem(args[1], tv, sizeof(tv)))
               r = -EFAULT;
         }
         break;
      }
      default:
         return false;
   }
   *ret = (uint64_t)r;
   return true;
}

bool can_block(int x86, const uint64_t args[6])
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

void note_wakes(threadid_t threadid, int x86, const uint64_t args[6])
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


namespace drtime
{

bool supported() { return DR_TIME; }

void init(thread_data_t *td, bool emulate_, uint64_t time_start_)
{
   thread_data = td;
   app_pid = (uint64_t)dr_get_process_id();
   emulate = emulate_;
   time_start = time_start_;
   if (!DR_TIME)
   {
      dr_fprintf(STDERR, "[SNIPER_FRONTEND] Warning: the guest's time is kept only on x86-64 under DynamoRIO: "
                         "here the guest reads the host's clocks and its timeouts and timers are the host kernel's\n");
      return;
   }
   clocks = new Clocks(MAX_THREADS, clock_instructions, NULL);
   timers = new Timers();
   sigwait_lock = new DrLock();
   clone_lock = new DrLock();
   clones = new std::vector<Clone>();
}

void exit()
{
   if (!DR_TIME)
      return;
   helper_stop = true;
   /* DynamoRIO has stopped the other threads, the client thread among them */
   delete clones;
   delete clone_lock;
   delete sigwait_lock;
   delete timers;
   delete clocks;
   clones = NULL;
   clone_lock = sigwait_lock = NULL;
   timers = NULL;
   clocks = NULL;
}

void thread_start(threadid_t threadid, uint64_t start_sp)
{
   if (!DR_TIME || threadid >= MAX_THREADS)
      return;
   int parent = -1;
   tid_words[threadid] = 0;
   clone_lock->lock();
   std::vector<Clone>::iterator it = clones->begin();
   for (; it != clones->end(); ++it)
      if (start_sp && it->stack == start_sp)
         break;
   if (it == clones->end() && !clones->empty())
      it = clones->begin();          /* stack unknown: the oldest */
   if (it != clones->end())
   {
      parent = it->parent;
      tid_words[threadid] = it->tid_word;
      clones->erase(it);
   }
   clone_lock->unlock();
   __atomic_store_n(&insns[threadid], 0, __ATOMIC_RELAXED);
   waiting[threadid] = false;
   timer_check_at[threadid] = 0;
   const uint64_t none[6] = { 0, 0, 0, 0, 0, 0 };
   host_tids[threadid] = host_syscall(X_gettid, none);
   clocks->start(threadid, threadid == 0 ? -1 : parent);
}

void thread_exit(threadid_t threadid)
{
   if (!DR_TIME || threadid >= MAX_THREADS)
      return;
   host_tids[threadid] = 0;
   clocks->exit(threadid, tid_words[threadid]);
}

void count(threadid_t threadid, uint64_t n)
{
   if (threadid < MAX_THREADS)
      __atomic_store_n(&insns[threadid], insns[threadid] + n, __ATOMIC_RELAXED);
}

bool flow(threadid_t threadid)
{
   if (!DR_TIME || threadid >= MAX_THREADS)
      return false;
   if (kicks[threadid] > 0)
   {
      kicks[threadid] = kicks[threadid] - 1;
      return true;                   /* a signal from the client thread waits */
   }
   if (!timers || !timers->armed() || !live(threadid))
      return false;
   const uint64_t n = insns[threadid];
   if (n - timer_check_at[threadid] < timer_check_interval())
      return false;
   timer_check_at[threadid] = n;
   return timers_check(threadid) != 0;
}

bool syscall(threadid_t threadid, int num, const uint64_t args[6], uint64_t *ret)
{
   if (!DR_TIME || threadid >= MAX_THREADS)
      return false;
   const int x86 = num;
   if (time_call(threadid, x86, args, ret))
      return true;
   if (x86 == X_clone || x86 == X_clone3)
      note_clone(threadid, x86, args);
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

void signal_delivered(threadid_t threadid)
{
   if (threadid < MAX_THREADS)
      kicks[threadid] = 0;
}

void syscall_returned(threadid_t threadid, int64_t ret)
{
   if (!DR_TIME || threadid >= MAX_THREADS || !waiting[threadid])
      return;
   waiting[threadid] = false;
   clocks->wait_end(threadid, ret == -EINTR ? Clocks::INTERRUPTED : Clocks::WOKEN);
}

bool clock_ns(threadid_t threadid, int64_t clk, uint64_t *ns)
{
   if (clk == 10 || clk > 11)
      return false;                  /* no such clock */
   const bool cputime = Sift::cputime_clock((int)clk);
   if (live(threadid))
   {
      /* the simulator's: GetTime from its osemu/time_start, Counters for
         the CPU-time clocks, from zero */
      Sift::EmuRequest req;
      Sift::EmuReply res;
      if (cputime && thread_data[threadid].output->Emulate(Sift::EmuTypeCounters, req, res))
      {
         *ns = res.counters.time_ns;
         return true;
      }
      if (!cputime && thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res))
      {
         *ns = res.gettime.time_ns;
         return true;
      }
   }
   *ns = one_ipc_clock(threadid, clk);
   return true;
}

uint64_t tsc(threadid_t threadid)
{
   if (live(threadid))
   {
      Sift::EmuRequest req;
      Sift::EmuReply res;
      if (thread_data[threadid].output->Emulate(Sift::EmuTypeRdtsc, req, res))
         return res.rdtsc.cycles;
   }
   /* the one-IPC model: the TSC counts time */
   return clocks->now(threadid) * Sift::ONE_IPC_HZ / Sift::NS_PER_S;
}

}  // namespace drtime
