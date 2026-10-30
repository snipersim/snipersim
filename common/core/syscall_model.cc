#include "syscall_model.h"
#include "config.h"
#include "simulator.h"
#include "syscall_server.h"
#include "thread.h"
#include "core.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "performance_model.h"
#include "instruction.h"
#include "pthread_emu.h"
#include "scheduler.h"
#include "hooks_manager.h"
#include "stats.h"
#include "syscall_strings.h"
#include "circular_log.h"
#include "thread_stats_manager.h"

#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "os_compat.h"

#include <boost/algorithm/string.hpp>

const char *SyscallMdl::futex_names[] =
{
   "FUTEX_WAIT", "FUTEX_WAKE", "FUTEX_FD", "FUTEX_REQUEUE",
   "FUTEX_CMP_REQUEUE", "FUTEX_WAKE_OP", "FUTEX_LOCK_PI", "FUTEX_UNLOCK_PI",
   "FUTEX_TRYLOCK_PI", "FUTEX_WAIT_BITSET", "FUTEX_WAKE_BITSET", "FUTEX_WAIT_REQUEUE_PI",
   "FUTEX_CMP_REQUEUE_PI"
};

SyscallMdl::SyscallMdl(Thread *thread)
      : m_thread(thread)
      , m_emulated(false)
      , m_stalled(false)
      , m_ret_val(0)
      , m_stdout_bytes(0)
      , m_stderr_bytes(0)
{
   UInt32 futex_counters_size = sizeof(struct futex_counters_t);
   __attribute__((unused)) int rc = posix_memalign((void**)&futex_counters, 64, futex_counters_size); // Align by cache line size to prevent thread contention
   LOG_ASSERT_ERROR (rc == 0, "posix_memalign failed to allocate memory");
   new (futex_counters) futex_counters_t();   // the raw memory constructed: zero counts and delays

   // Register the metrics
   for (unsigned int e = 0; e < sizeof(futex_names) / sizeof(futex_names[0]); e++)
   {
      registerStatsMetric("futex", thread->getId(), boost::to_lower_copy(String(futex_names[e]) + "_count"), &(futex_counters->count[e]));
      registerStatsMetric("futex", thread->getId(), boost::to_lower_copy(String(futex_names[e]) + "_delay"), &(futex_counters->delay[e]));
   }

   registerStatsMetric("syscall", thread->getId(), "stdout-bytes", &m_stdout_bytes);
   registerStatsMetric("syscall", thread->getId(), "stderr-bytes", &m_stderr_bytes);
}

SyscallMdl::~SyscallMdl()
{
   free(futex_counters);
}

IntPtr SyscallMdl::cpuClockTime(Thread *thread, int clock, UInt64 *ns)
{
   /* A dynamic id: ~pid << 3 | type, with bit 2 (CPUCLOCK_PERTHREAD) set
      for a thread's clock; type 3 is none.  Pid (tid) 0 is the caller's
      own.  A thread is found by the tid its front end reported; while a
      front end reports none (the caller's tid unknown), the caller's. */
   bool per_thread = clock == CLOCK_THREAD_CPUTIME_ID;
   Thread *target = thread;
   if (clock < 0)
   {
      if ((clock & 3) == 3)
         return -EINVAL;
      per_thread = (clock & 4) != 0;
      const pid_t pid = ~(clock >> 3);
      if (per_thread && pid != 0 && thread->m_os_info.tid != 0 && pid != thread->m_os_info.tid)
      {
         ScopedLock sl(Sim()->getThreadManager()->getLock());
         target = Sim()->getThreadManager()->findThreadByTid(pid);
         if (!target || target->getAppId() != thread->getAppId())
            return -EINVAL;
      }
   }
   else if (clock != CLOCK_PROCESS_CPUTIME_ID && clock != CLOCK_THREAD_CPUTIME_ID)
      return -EINVAL;

   ScopedLock sl(Sim()->getThreadManager()->getLock());
   *ns = (per_thread ? Sim()->getThreadStatsManager()->getThreadCpuTime(target->getId())
                     : Sim()->getThreadStatsManager()->getAppCpuTime(thread->getAppId())).getNS();
   return 0;
}

bool SyscallMdl::runEnter(IntPtr syscall_number, syscall_args_t &args)
{
   Core *core = m_thread->getCore();
   LOG_ASSERT_ERROR(core != NULL, "Syscall by thread %d: core should not be null", m_thread->getId());

   LOG_PRINT("Got Syscall: %i", syscall_number);
   CLOG("syscall", "Enter thread %d core %d syscall %" PRIdPTR, m_thread->getId(), core->getId(), syscall_number);

   m_syscall_number = syscall_number;
   m_in_syscall = true;
   m_syscall_args = args;

   HookSyscallEnter hook_args;
   hook_args.thread_id = m_thread->getId();
   hook_args.core_id = core->getId();
   hook_args.time = core->getPerformanceModel()->getElapsedTime();
   hook_args.syscall_number = syscall_number;
   hook_args.args = args;
   {
      ScopedLock sl(Sim()->getThreadManager()->getLock());
      Sim()->getHooksManager()->callHooks(HookType::HOOK_SYSCALL_ENTER, (UInt64)&hook_args);
   }

   switch (syscall_number)
   {
      case SYS_futex:
         m_ret_val = handleFutexCall(args);
         m_emulated = true;
         break;

      case SYS_clock_gettime:
      case SYS_gettimeofday:
      {
         /* The time in simulated seconds: emulation start plus the core's
            elapsed time (osemu/clock_replace).  Only a front end without
            routine replacement sends these -- the QEMU plugin, whose
            guest's libc reaches them through QEMU's vDSO, which makes the
            system call; the Pin recorder replaces the libc routines
            instead (sift/recorder/emulation.cc) and never forwards them.

            Once sent, the call is answered here whatever the clock: the
            front end skips it and takes this answer.  Every clock is
            simulated time: the CPU-time clocks (the process's, the thread's,
            and the dynamic ones, negative ids) count from zero, as CPU time
            does, every other clock from osemu/time_start.  Only with
            clock_replace off is the answer the host's clock, as the guest
            would have read it.  args.arg1 (args.arg0 for gettimeofday) is a
            guest address: written through the core, as nanosleep reads its
            timespec. */
         const bool is_tod = syscall_number == SYS_gettimeofday;
         const clockid_t clock = is_tod ? CLOCK_REALTIME : (clockid_t)args.arg0;
         const IntPtr out = is_tod ? (IntPtr)args.arg0 : (IntPtr)args.arg1;
         const bool simulated = Sim()->getConfig()->getOSEmuClockReplace();
         const bool cputime = isCpuClock(clock);
         struct timespec ts;
         /* The kernel's answers to what it refuses: a clock id it does not
            know (0-11 but 10, CLOCK_SGI_CYCLE, removed; a dynamic CPU clock
            is checked by cpuClockTime), a NULL timespec */
         if (!is_tod && !cputime && (clock > 11 || clock == 10))
            m_ret_val = -EINVAL;
         else if (!is_tod && !out)
            m_ret_val = -EFAULT;
         else if (simulated)
         {
            /* The CPU-time clocks count the time the thread ran (the
               process's: its threads'), not the time it slept or waited */
            UInt64 time_ns = 0;
            if (cputime)
               m_ret_val = cpuClockTime(m_thread, clock, &time_ns);
            else
            {
               time_ns = Sim()->getConfig()->getOSEmuTimeStart() * 1000000000
                       + m_thread->getCore()->getPerformanceModel()->getElapsedTime().getNS();
               m_ret_val = 0;
            }
            ts.tv_sec = time_ns / 1000000000;
            ts.tv_nsec = time_ns % 1000000000;
         }
         else
         {
            m_ret_val = clock_gettime(clock, &ts) == 0 ? 0 : (IntPtr)-errno;
         }
         if (m_ret_val == 0 && out)
         {
            if (is_tod)
            {
               struct timeval tv;
               tv.tv_sec = ts.tv_sec;
               tv.tv_usec = ts.tv_nsec / 1000;
               core->accessMemory(Core::NONE, Core::WRITE, out, (char*) &tv, sizeof(tv));
            }
            else
               core->accessMemory(Core::NONE, Core::WRITE, out, (char*) &ts, sizeof(ts));
         }
         if (m_ret_val == 0 && is_tod && args.arg1)
         {
            /* gettimeofday's timezone: UTC, no DST, as the kernel's default */
            struct timezone tz;
            tz.tz_minuteswest = 0;
            tz.tz_dsttime = 0;
            core->accessMemory(Core::NONE, Core::WRITE, (IntPtr)args.arg1, (char*) &tz, sizeof(tz));
         }
         m_emulated = true;
         break;
      }

      case SYS_nanosleep:
      case SYS_clock_nanosleep:
      {
         /* A sleep in simulated time: the thread is parked until the barrier
            has advanced the clock to its wake-up time, so the sleep costs
            exactly what it asked for, however long the host takes.

            Both entries matter.  usleep(3), sleep(3) and nanosleep(3) all
            reach the kernel as clock_nanosleep since glibc 2.31, and the
            frontend only emulates what is listed here.  Left native, the
            thread sleeps in host time while the simulator believes it is
            running: a thread that only ever sleeps and does kernel-mode
            (untimed) work never moves its core's clock, and every other core
            then waits at the barrier for one that cannot arrive. */
         const struct timespec *req;
         struct timespec *rem;
         bool absolute = false;
         clockid_t clock = CLOCK_REALTIME;
         if (syscall_number == SYS_nanosleep)
         {
            req = (const struct timespec *)args.arg0;
            rem = (struct timespec *)args.arg1;
         }
         else
         {
            clock = (clockid_t)args.arg0;
            absolute = ((int)args.arg1 & TIMER_ABSTIME) != 0;
            req = (const struct timespec *)args.arg2;
            rem = (struct timespec *)args.arg3;
         }
         Core *core = m_thread->getCore();
         SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime();

         struct timespec local_req;
         core->accessMemory(Core::NONE, Core::READ, (IntPtr) req, (char*) &local_req, sizeof(local_req));

         SubsecondTime time_wake;
         if (!absolute)
         {
            time_wake = start_time + SubsecondTime::SEC(local_req.tv_sec) + SubsecondTime::NS(local_req.tv_nsec);
         }
         else if (Sim()->getConfig()->getOSEmuClockReplace())
         {
            /* An absolute deadline on the clock the guest reads through
               SYS_clock_gettime above: emulation start plus elapsed time. */
            SubsecondTime deadline = SubsecondTime::SEC(local_req.tv_sec) + SubsecondTime::NS(local_req.tv_nsec);
            SubsecondTime base = SubsecondTime::SEC(Sim()->getConfig()->getOSEmuTimeStart());
            time_wake = deadline > base ? deadline - base : SubsecondTime::Zero();
            if (time_wake < start_time)
               time_wake = start_time;
         }
         else
         {
            /* The guest took its deadline from the host clock; only the host
               knows how far away it is. */
            struct timespec now;
            clock_gettime(clock, &now);
            SInt64 remaining_ns = (SInt64)(local_req.tv_sec - now.tv_sec) * 1000000000 + (local_req.tv_nsec - now.tv_nsec);
            time_wake = start_time + (remaining_ns > 0 ? SubsecondTime::NS(remaining_ns) : SubsecondTime::Zero());
         }

         SubsecondTime end_time;
         Sim()->getSyscallServer()->handleSleepCall(m_thread->getId(), time_wake, start_time, end_time);

         if (m_thread->reschedule(end_time, core))
            core = m_thread->getCore();

         if (core)      // none when the application ended during the sleep (Thread::reschedule)
            core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::SLEEP));

         if (rem && !absolute && core)
         {
            // Interruption not supported, always return 0 remaining time
            struct timespec local_rem;
            local_rem.tv_sec = 0;
            local_rem.tv_nsec = 0;
            core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) rem, (char*) &local_rem, sizeof(local_rem));
         }

         // Always succeeds
         m_ret_val = 0;
         m_emulated = true;

         break;
      }

      case SYS_read:
      case SYS_pause:
      case SYS_select:
      case SYS_poll:
      case SYS_wait4:
      {
         // System call is blocking, mark thread as asleep
         ScopedLock sl(Sim()->getThreadManager()->getLock());
         Sim()->getThreadManager()->stallThread_async(m_thread->getId(),
                                                      syscall_number == SYS_pause ? ThreadManager::STALL_PAUSE : ThreadManager::STALL_SYSCALL,
                                                      m_thread->getCore()->getPerformanceModel()->getElapsedTime());
         m_stalled = true;
         break;
      }

      case SYS_sched_yield:
      {
         {
            ScopedLock sl(Sim()->getThreadManager()->getLock());
            Sim()->getThreadManager()->getScheduler()->threadYield(m_thread->getId());
         }

         // We may have been rescheduled
         SubsecondTime time = core->getPerformanceModel()->getElapsedTime();
         if (m_thread->reschedule(time, core))
            core = m_thread->getCore();
         core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::UNSCHEDULED));

         // Always succeeds
         m_ret_val = 0;
         m_emulated = true;

         break;
      }

      case SYS_sched_setaffinity:
      {
         pid_t pid = (pid_t)args.arg0;
         size_t cpusetsize = (size_t)args.arg1;
         const cpu_set_t *cpuset = (const cpu_set_t *)args.arg2;
         Thread *thread;
         bool success = false;

         if (cpuset == NULL)
         {
            m_ret_val = -EFAULT;
            m_emulated = true;
            break;
         }

         if (pid == 0)
            thread = m_thread;
         else
            thread = Sim()->getThreadManager()->findThreadByTid(pid);

         if (thread)
         {
            char *local_cpuset = new char[cpusetsize];
            core->accessMemory(Core::NONE, Core::READ, (IntPtr) cpuset, local_cpuset, cpusetsize);

            ScopedLock sl(Sim()->getThreadManager()->getLock());
            success = Sim()->getThreadManager()->getScheduler()->threadSetAffinity(m_thread->getId(), thread->getId(), cpusetsize, (cpu_set_t *)local_cpuset);

            delete [] local_cpuset;
         }
         else
         {
            m_ret_val = -ESRCH;
            m_emulated = true;
            break;
         }

         // We may have been rescheduled
         SubsecondTime time = core->getPerformanceModel()->getElapsedTime();
         if (m_thread->reschedule(time, core))
            core = m_thread->getCore();
         core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::UNSCHEDULED));

         m_ret_val = success ? 0 : -EINVAL;
         m_emulated = true;

         break;
      }

      case SYS_sched_getaffinity:
      {
         pid_t pid = (pid_t)args.arg0;
         size_t cpusetsize = (size_t)args.arg1;
         cpu_set_t *cpuset = (cpu_set_t *)args.arg2;
         Thread *thread;
         bool success = false;

         if (pid == 0)
            thread = m_thread;
         else
            thread = Sim()->getThreadManager()->findThreadByTid(pid);

         if (thread)
         {
            char *local_cpuset = cpuset ? new char[cpusetsize] : 0;

            ScopedLock sl(Sim()->getThreadManager()->getLock());
            success = Sim()->getThreadManager()->getScheduler()->threadGetAffinity(m_thread->getId(), cpusetsize, (cpu_set_t *)local_cpuset);

            if (success && cpuset)
               core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) cpuset, local_cpuset, cpusetsize);
            if (local_cpuset)
               delete [] local_cpuset;
         }
         // else: success is already false, return EINVAL for invalid pid

         // On success: return size of affinity mask (in bytes) needed to represent however many cores we're modeling
         // (returning a value that is too large can cause a segfault in the application's libc)
         //
         // Both arms are widened to IntPtr before the conditional chooses between
         // them.  Without that, getApplicationCores() returns UInt32 and drags the
         // whole expression to unsigned int, so the failure arm -- -EINVAL -- was
         // converted to 4294967274 and then zero-extended into m_ret_val.  The
         // application received that as the return value of sched_getaffinity, and
         // glibc's wrapper does
         //
         //     if (res != -1) memset ((char *) cpuset + res, 0, cpusetsize - res);
         //
         // so a huge positive "success" made cpusetsize - res underflow and memset
         // a colossal region, destroying the heap:
         //
         //     free(): invalid pointer
         //
         // which killed the process partway through simulation.  The comment above
         // was already warning about exactly this failure mode.
         m_ret_val = success ? (IntPtr)((Sim()->getConfig()->getApplicationCores()+7)/8)
                             : (IntPtr)(-EINVAL);
         m_emulated = true;

         break;
      }

      case SYS_write:
      {
         int fd = (int)args.arg0;
         size_t count = (size_t)args.arg2;

         if ((fd == STDOUT_FILENO && Sim()->getConfig()->suppressStdout()) || (fd == STDERR_FILENO && Sim()->getConfig()->suppressStderr()))
         {
            m_ret_val = count;
            m_emulated = true;
         }

         if (fd == STDOUT_FILENO)
            m_stdout_bytes += count;
         if (fd == STDERR_FILENO)
            m_stderr_bytes += count;

         break;
      }

      case static_cast<unsigned long>(-1):
      default:
         break;
   }

   LOG_PRINT("Syscall finished");
   CLOG("syscall", "Finished thread %d", m_thread->getId());

   return m_stalled;
}

IntPtr SyscallMdl::runExit(IntPtr old_return)
{
   CLOG("syscall", "Exit thread %d", m_thread->getId());

   if (m_stalled)
   {
      SubsecondTime time_wake = Sim()->getClockSkewMinimizationServer()->getGlobalTime(true /*upper_bound*/);

      {
         // System call is blocking, mark thread as awake
         ScopedLock sl(Sim()->getThreadManager()->getLock());
         Sim()->getThreadManager()->resumeThread_async(m_thread->getId(), INVALID_THREAD_ID, time_wake, NULL);
      }

      Core *core = Sim()->getCoreManager()->getCurrentCore();
      m_thread->reschedule(time_wake, core);
      core = m_thread->getCore();

      if (core)      // none when the application ended meanwhile (Thread::reschedule)
         core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time_wake,
            m_syscall_number == SYS_pause ? SyncInstruction::PAUSE : SyncInstruction::SYSCALL));

      m_stalled = false;
   }

   if (!m_emulated)
   {
      m_ret_val = old_return;
   }

   Core *core = m_thread->getCore();
   if (!core)
   {
      // The application ended while this thread waited, and it gave up its
      // wait for a core (Thread::reschedule): nothing to time or report.
      m_emulated = false;
      m_in_syscall = false;
      return m_ret_val;
   }
   HookSyscallExit hook_args;
   hook_args.thread_id = m_thread->getId();
   hook_args.core_id = core->getId();
   hook_args.time = core->getPerformanceModel()->getElapsedTime();
   hook_args.ret_val = m_ret_val;
   hook_args.emulated = m_emulated;
   {
      ScopedLock sl(Sim()->getThreadManager()->getLock());
      Sim()->getHooksManager()->callHooks(HookType::HOOK_SYSCALL_EXIT, (UInt64)&hook_args);
   }

   m_emulated = false;
   m_in_syscall = false;

   return m_ret_val;
}

void SyscallMdl::futexCount(uint32_t function, SubsecondTime delay)
{
   futex_counters->count[function]++;
   futex_counters->delay[function] += delay;
}

IntPtr SyscallMdl::handleFutexCall(syscall_args_t &args)
{
   SyscallServer::futex_args_t fargs;
   fargs.uaddr = (int*) args.arg0;
   fargs.op = (int) args.arg1;
   fargs.val = (int) args.arg2;
   fargs.timeout = (const struct timespec*) args.arg3;
   fargs.val2 = (int) args.arg3;
   fargs.uaddr2 = (int*) args.arg4;
   fargs.val3 = (int) args.arg5;

   int cmd = (fargs.op & FUTEX_CMD_MASK) & ~FUTEX_PRIVATE_FLAG;

   Core *core = m_thread->getCore();
   LOG_ASSERT_ERROR(core != NULL, "Syscall by thread %d: core should not be null", m_thread->getId());

   SubsecondTime start_time;
   SubsecondTime end_time;
   start_time = core->getPerformanceModel()->getElapsedTime();

   updateState(core, PthreadEmu::STATE_WAITING);

   IntPtr ret_val = Sim()->getSyscallServer()->handleFutexCall(m_thread->getId(), fargs, start_time, end_time);

   if (m_thread->reschedule(end_time, core))
      core = m_thread->getCore();

   if (!core)      // the application ended while this thread waited (Thread::reschedule)
      return ret_val;

   core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::FUTEX));

   SubsecondTime delay = end_time - start_time;

   updateState(core, PthreadEmu::STATE_RUNNING, delay);

   // Update the futex statistics
   futexCount(cmd, delay);

   return ret_val;
}

String SyscallMdl::formatSyscall() const
{
   return String(syscall_string(m_syscall_number)) + "[" + itostr(m_syscall_number) + "] ("
      + itostr(m_syscall_args.arg0) + ", " + itostr(m_syscall_args.arg1) + ", " + itostr(m_syscall_args.arg2) + ", "
      + itostr(m_syscall_args.arg3) + ", " + itostr(m_syscall_args.arg4) + ", " + itostr(m_syscall_args.arg5) + ")";
}
