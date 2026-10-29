#ifndef __SIFT_GUEST_TIME_H
#define __SIFT_GUEST_TIME_H

/* The guest's timeouts, in the front ends.
 *
 * No time the guest gives reaches the host's kernel: a timeout, a deadline,
 * a sleep is the guest's, in the guest's time, and the host's kernel would
 * count it in host seconds -- a 10 ms wait of a guest running a thousand
 * times slower than the host ended after 10 us of the guest's time, and a
 * deadline in the guest's clock (which starts at Sift::TIME_START_DEFAULT)
 * was 14 years in the host's past or future.  Where the simulator answers (a
 * live run), it keeps the time: it models futex, the sleeps and the clocks,
 * and a front end runs every other timed wait as probes without a timeout,
 * between which the simulator lets the thread sleep in simulated time.
 * Where it does not (a trace written to a file), the front end keeps the
 * time itself, with the one-IPC model (GuestClocks below).
 *
 * This header says which calls carry a timeout and where (TimedCall), and
 * holds the one-IPC model's clocks; the front ends do the rest.  System
 * calls are numbered as on x86-64 (the front ends map other guests' numbers
 * to these, as for the calls they send to the simulator).  Plain C++, no
 * library: it builds under PinCRT too. */

#include <stdint.h>
#include "sift_format.h"

namespace Sift
{
   /* Where a call's timeout is and what it means */
   enum TimeoutForm
   {
      TO_NONE,          /* no timeout: the call does not wait on time */
      TO_TIMESPEC,      /* a struct timespec * argument (NULL: none) */
      TO_TIMEVAL,       /* a struct timeval * (select; written back with the time left) */
      TO_MS,            /* an int of milliseconds in the argument (negative: none) */
      TO_SECONDS,       /* alarm's unsigned int of seconds */
   };

   enum TimedClass
   {
      TC_NONE,          /* not a timed call */
      TC_SLEEP,         /* nanosleep, clock_nanosleep: only time ends it */
      TC_WAIT,          /* waits for an event or a timeout: futex, poll, ... */
   };

   struct TimedCall
   {
      TimedClass cls;
      TimeoutForm form;
      int arg;          /* the argument the timeout is in */
      bool absolute;    /* a deadline on a clock rather than a duration */
      int clock;        /* the deadline's clock (CLOCK_REALTIME 0, CLOCK_MONOTONIC 1, ...) */
      int rem_arg;      /* nanosleep's and clock_nanosleep's time-left argument, or -1 */
      bool fdsets;      /* select, pselect6: arguments 1-3 are fd sets the call rewrites */
   };

   /* x86-64 numbers of the calls below */
   enum
   {
      X86_poll = 7, X86_select = 23, X86_nanosleep = 35, X86_alarm = 37,
      X86_rt_sigtimedwait = 128, X86_futex = 202, X86_io_getevents = 208,
      X86_semtimedop = 220, X86_clock_nanosleep = 230, X86_epoll_wait = 232,
      X86_mq_timedsend = 242, X86_mq_timedreceive = 243, X86_pselect6 = 270,
      X86_ppoll = 271, X86_epoll_pwait = 281, X86_recvmmsg = 299,
      X86_io_pgetevents = 333, X86_epoll_pwait2 = 441, X86_futex_waitv = 449,
   };

   /* futex: the operations that wait, and their timeouts: FUTEX_WAIT's is a
      duration, the others' a deadline on CLOCK_MONOTONIC, or CLOCK_REALTIME
      with FUTEX_CLOCK_REALTIME (LOCK_PI's always on CLOCK_REALTIME) */
   inline bool futex_timed_op(uint64_t op, bool *absolute, int *clock)
   {
      const unsigned cmd = op & 127, realtime = (op & 256) != 0;
      switch (cmd)
      {
         case 0:  *absolute = false; *clock = 1; return true;            /* FUTEX_WAIT */
         case 9:                                                         /* FUTEX_WAIT_BITSET */
         case 11:                                                        /* FUTEX_WAIT_REQUEUE_PI */
         case 13: *absolute = true; *clock = realtime ? 0 : 1; return true;  /* FUTEX_LOCK_PI2 */
         case 6:  *absolute = true; *clock = 0; return true;            /* FUTEX_LOCK_PI */
         default: return false;
      }
   }

   /* The call's timeout, if it has one.  args as the guest passes them. */
   inline bool timed_call(int x86_num, const uint64_t args[6], TimedCall *tc)
   {
      TimedCall t = { TC_WAIT, TO_TIMESPEC, 0, false, 1, -1, false };
      switch (x86_num)
      {
         case X86_nanosleep:       t.cls = TC_SLEEP; t.arg = 0; t.rem_arg = 1; break;
         case X86_clock_nanosleep: t.cls = TC_SLEEP; t.arg = 2; t.rem_arg = 3;
                                   t.absolute = (args[1] & 1) != 0; t.clock = (int)args[0]; break;   /* TIMER_ABSTIME */
         case X86_futex:
            if (!futex_timed_op(args[1], &t.absolute, &t.clock))
               return false;
            t.arg = 3;
            break;
         case X86_poll:            t.form = TO_MS; t.arg = 2; break;
         case X86_epoll_wait:
         case X86_epoll_pwait:     t.form = TO_MS; t.arg = 3; break;
         case X86_epoll_pwait2:    t.arg = 3; break;
         case X86_select:          t.form = TO_TIMEVAL; t.arg = 4; t.fdsets = true; break;
         case X86_pselect6:        t.arg = 4; t.fdsets = true; break;
         case X86_ppoll:           t.arg = 2; break;
         case X86_rt_sigtimedwait: t.arg = 2; break;
         case X86_semtimedop:      t.arg = 3; break;
         case X86_io_getevents:
         case X86_io_pgetevents:   t.arg = 4; break;
         case X86_recvmmsg:        t.arg = 4; break;
         case X86_mq_timedsend:
         case X86_mq_timedreceive: t.arg = 4; t.absolute = true; t.clock = 0; break;
         case X86_futex_waitv:     t.arg = 3; t.absolute = true; t.clock = (int)args[4]; break;
         default: return false;
      }
      *tc = t;
      return true;
   }

   /* Does the call wait without end?  (No timeout given: a NULL pointer, a
      negative number of milliseconds.)  Zero waits not at all. */
   inline bool timeout_infinite(const TimedCall &t, const uint64_t args[6])
   {
      if (t.form == TO_MS)
         return (int32_t)args[t.arg] < 0;
      return args[t.arg] == 0;
   }

   inline bool cputime_clock(int clock)
   {
      return clock == 2 || clock == 3 || clock < 0;   /* the process's, the thread's, dynamic ones */
   }

   const uint64_t NO_DEADLINE = ~0ull;

   /* The one-IPC model's clocks, where the simulator does not answer.
    *
    * A thread's clock is its instructions at Sift::ONE_IPC_HZ, from the time
    * it started at -- its parent's time then (the first thread's: zero), so
    * that a child does not start in its parent's past -- plus the time it
    * waited.  Every clock but the CPU-time ones reads TIME_START_DEFAULT
    * (osemu/time_start) seconds plus that.
    *
    * A timed wait ends at its deadline in this time, never in the host's:
    * when every running thread's clock has passed it (none of them can wake
    * the waiter earlier any more), or, when no thread runs (all wait), the
    * earliest deadline is reached first -- the time jumps to it, as in a
    * simulator with nothing to do.  A thread woken takes the waker's time if
    * it is later than its own: the latest wake on its futex word (a thread's
    * exit wakes its joiner on its tid word), or, for any other wait (a read,
    * a poll), the latest wake of any kind (a write, a signal) -- a later time
    * than the real waker's at worst, never an earlier one.  A thread in any call that can block counts as waiting, so that a
    * thread blocked in a read or a join does not hold up another's
    * timeout.  The front end supplies each thread's instruction count
    * (Instructions) and the lock (Lock: lock(), unlock()); slots are its
    * thread numbers, below max. */
   template <class Lock>
   class GuestClocks
   {
   public:
      typedef uint64_t (*Instructions)(unsigned slot, void *arg);

      GuestClocks(unsigned max, Instructions instructions, void *arg)
         : m_instructions(instructions), m_arg(arg), m_max(max), m_t(new Thread[max]),
           m_wake(0), m_exited_insns(0), m_epoch(0)
      {
         for (unsigned i = 0; i < WORDS; i++)
            m_words[i].word = m_words[i].time = 0;
      }

      /* A thread starts, at its parent's time (parent: a slot, or -1) */
      void start(unsigned slot, int parent)
      {
         m_lock.lock();
         const uint64_t at = parent >= 0 && (unsigned)parent < m_max && m_t[parent].used ? now_locked(parent) : 0;
         m_t[slot] = Thread();
         m_t[slot].used = true;
         m_t[slot].offset = at;
         m_epoch++;
         m_lock.unlock();
      }

      /* A thread ends: a joiner woken by it (on tid_word) takes its time */
      void exit(unsigned slot, uint64_t tid_word)
      {
         m_lock.lock();
         if (m_t[slot].used)
         {
            woke_locked(now_locked(slot), tid_word);
            m_exited_insns += m_instructions(slot, m_arg);
            m_t[slot].used = false;
            m_epoch++;
         }
         m_lock.unlock();
      }

      /* The thread's time: nanoseconds since its clock's zero */
      uint64_t now(unsigned slot)
      {
         m_lock.lock();
         const uint64_t t = now_locked(slot);
         m_lock.unlock();
         return t;
      }

      /* The thread's time after a given number of its instructions (a read
         within a block, which the count has passed already) */
      uint64_t at(unsigned slot, uint64_t instructions)
      {
         m_lock.lock();
         const uint64_t t = m_t[slot].offset + one_ipc_ns(instructions);
         m_lock.unlock();
         return t;
      }

      /* The CPU-time clocks: the thread's instructions, the process's */
      uint64_t thread_cpu(unsigned slot) { return one_ipc_ns(m_instructions(slot, m_arg)); }
      uint64_t process_cpu()
      {
         m_lock.lock();
         uint64_t n = m_exited_insns;
         for (unsigned i = 0; i < m_max; i++)
            if (m_t[i].used)
               n += m_instructions(i, m_arg);
         m_lock.unlock();
         return one_ipc_ns(n);
      }

      /* A thread wakes others now: a futex wake on word (a futex word's
         address), or anything else (word 0: a write, a signal) */
      void woke(unsigned slot, uint64_t word)
      {
         m_lock.lock();
         woke_locked(now_locked(slot), word);
         m_lock.unlock();
      }

      /* A thread waits (a call that can block), until deadline (its time),
         or NO_DEADLINE; on futex word, or 0 */
      void wait_begin(unsigned slot, uint64_t deadline, uint64_t word)
      {
         m_lock.lock();
         m_t[slot].waiting = true;
         m_t[slot].deadline = deadline;
         m_t[slot].word = word;
         m_epoch++;
         m_lock.unlock();
      }

      /* May the waiting thread's deadline pass now?  Returns 1 when every
         running thread is past it, 2 when no thread runs and it is the
         earliest deadline (the caller should see this twice, with the same
         epoch, a moment apart: a thread that was only passing through a
         call has returned by then), 0 otherwise.  *epoch: the state's
         version, for that second look. */
      int may_expire(unsigned slot, uint64_t *epoch)
      {
         m_lock.lock();
         const uint64_t d = m_t[slot].deadline;
         bool running = false;
         uint64_t low = NO_DEADLINE, earliest = NO_DEADLINE;
         for (unsigned i = 0; i < m_max; i++)
         {
            if (!m_t[i].used || i == slot)
               continue;
            if (m_t[i].waiting)
            {
               if (m_t[i].deadline < earliest)
                  earliest = m_t[i].deadline;
            }
            else
            {
               running = true;
               const uint64_t t = now_locked(i);
               if (t < low)
                  low = t;
            }
         }
         int r = 0;
         if (d != NO_DEADLINE)
         {
            if (running)
               r = low >= d ? 1 : 0;
            else
               r = d <= earliest ? 2 : 0;
         }
         *epoch = m_epoch;
         m_lock.unlock();
         return r;
      }

      enum End { WOKEN, TIMED_OUT, INTERRUPTED };

      /* The wait ended: at its deadline, or woken -- then at its waker's
         time, if later than the thread's own -- or interrupted by a signal
         (the thread's own time) */
      void wait_end(unsigned slot, End how)
      {
         m_lock.lock();
         Thread &t = m_t[slot];
         const uint64_t own = now_locked(slot);
         const bool timed_out = how == TIMED_OUT;
         uint64_t to = how == INTERRUPTED ? own : timed_out ? t.deadline : wake_time(t.word);
         if (!timed_out && t.deadline != NO_DEADLINE && to > t.deadline)
            to = t.deadline;
         if (to != NO_DEADLINE && to > own)
            t.offset += to - own;
         t.waiting = false;
         t.deadline = NO_DEADLINE;
         m_epoch++;
         m_lock.unlock();
      }

   private:
      struct Thread
      {
         bool used, waiting;
         uint64_t offset, deadline, word;
         Thread() : used(false), waiting(false), offset(0), deadline(NO_DEADLINE), word(0) {}
      };

      uint64_t now_locked(unsigned slot) { return m_t[slot].offset + one_ipc_ns(m_instructions(slot, m_arg)); }

      /* The wakes' times: the latest of any kind, and per futex word in a
         small table -- a word whose entry another took falls back to the
         latest of any kind, later, never earlier */
      enum { WORDS = 256 };
      struct Wake { uint64_t word, time; };
      static unsigned word_hash(uint64_t w) { return (unsigned)((w >> 2) ^ (w >> 10) ^ (w >> 18)) & (WORDS - 1); }

      void woke_locked(uint64_t t, uint64_t word)
      {
         if (t > m_wake)
            m_wake = t;
         if (word)
         {
            Wake &e = m_words[word_hash(word)];
            if (e.word != word)
            {
               e.word = word;
               e.time = t;
            }
            else if (t > e.time)
               e.time = t;
         }
      }

      uint64_t wake_time(uint64_t word) const
      {
         if (word)
         {
            const Wake &e = m_words[word_hash(word)];
            if (e.word == word)
               return e.time;
         }
         return m_wake;
      }

      Instructions m_instructions;
      void *m_arg;
      Lock m_lock;
      unsigned m_max;
      Thread *m_t;
      Wake m_words[WORDS];
      uint64_t m_wake;           /* the latest time a thread woke others at */
      uint64_t m_exited_insns;   /* the instructions of the threads that ended */
      uint64_t m_epoch;
   };
}

#endif // __SIFT_GUEST_TIME_H
