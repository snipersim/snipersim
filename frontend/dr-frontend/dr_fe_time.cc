#include "dr_api.h"
#include "dr_fe_time.h"
#include "guest_time.h"
#include "sift_writer.h"

#include <string.h>
#include <errno.h>
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
 * never the host's.  The SIFT recorder's emulation.cc and
 * syscall_modeling.cc, for DynamoRIO.
 *
 * The clocks -- clock_gettime, gettimeofday and time, the vDSO's routines
 * (replaced, dr_frontend.cc) and the system calls (time_call), and rdtsc --
 * are the simulator's, live (a simulator answering: -e, the thread's
 * channel open), and otherwise the one-IPC model's (Sift::GuestClocks:
 * a thread's instructions at 1 GHz from its parent's time at its start),
 * from the time zero (-time_start, osemu/time_start).
 *
 * x86-64 numbers; another ISA is refused (init). */

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

/* x86-64 numbers of the calls recognised here */
enum
{
   X_clone = 56, X_gettimeofday = 96, X_time = 201, X_clock_gettime = 228, X_clone3 = 435,
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

}  // namespace


namespace drtime
{

bool supported() { return DR_TIME; }

void init(thread_data_t *td, bool emulate_, uint64_t time_start_)
{
   thread_data = td;
   emulate = emulate_;
   time_start = time_start_;
   if (!DR_TIME)
   {
      dr_fprintf(STDERR, "[SNIPER_FRONTEND] Warning: the guest's time is kept only on x86-64 under DynamoRIO: "
                         "here the guest reads the host's clocks\n");
      return;
   }
   clocks = new Clocks(MAX_THREADS, clock_instructions, NULL);
   clone_lock = new DrLock();
   clones = new std::vector<Clone>();
}

void exit()
{
   if (!DR_TIME)
      return;
   delete clones;
   delete clone_lock;
   delete clocks;
   clones = NULL;
   clone_lock = NULL;
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
   clocks->start(threadid, threadid == 0 ? -1 : parent);
}

void thread_exit(threadid_t threadid)
{
   if (!DR_TIME || threadid >= MAX_THREADS)
      return;
   clocks->exit(threadid, tid_words[threadid]);
}

void count(threadid_t threadid, uint64_t n)
{
   if (threadid < MAX_THREADS)
      __atomic_store_n(&insns[threadid], insns[threadid] + n, __ATOMIC_RELAXED);
}

bool syscall(threadid_t threadid, int num, const uint64_t args[6], uint64_t *ret)
{
   if (!DR_TIME || threadid >= MAX_THREADS)
      return false;
   if (time_call(threadid, num, args, ret))
      return true;
   if (num == X_clone || num == X_clone3)
      note_clone(threadid, num, args);
   return false;
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
