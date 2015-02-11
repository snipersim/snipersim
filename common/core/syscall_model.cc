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

#include <errno.h>
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
   bzero(futex_counters, futex_counters_size);

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
      {
         if (Sim()->getConfig()->getOSEmuClockReplace())
         {
            clockid_t clock = (clock_t)args.arg0;
            struct timespec *ts = (struct timespec *)args.arg1;
            UInt64 time_ns = Sim()->getConfig()->getOSEmuTimeStart() * 1000000000
                           + m_thread->getCore()->getPerformanceModel()->getElapsedTime().getNS();

            switch(clock) {
               case CLOCK_REALTIME:
               case CLOCK_MONOTONIC:
               case CLOCK_MONOTONIC_COARSE:
               case CLOCK_MONOTONIC_RAW:
                  ts->tv_sec = time_ns / 1000000000;
                  ts->tv_nsec = time_ns % 1000000000;
                  m_ret_val = 0;
                  break;
               default:
                  LOG_ASSERT_ERROR(false, "SYS_clock_gettime does not currently support clock(%u)", clock);
            }
            m_emulated = true;
         }
         // else: don't set m_emulated and the system call will be executed natively
         break;
      }

      case SYS_nanosleep:
      {
         const struct timespec *req = (struct timespec *)args.arg0;
         struct timespec *rem = (struct timespec *)args.arg1;
         Core *core = m_thread->getCore();
         SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime();

         struct timespec local_req;
         core->accessMemory(Core::NONE, Core::READ, (IntPtr) req, (char*) &local_req, sizeof(local_req));

         SubsecondTime time_wake = start_time + SubsecondTime::SEC(local_req.tv_sec) + SubsecondTime::NS(local_req.tv_nsec);
         SubsecondTime end_time;
         Sim()->getSyscallServer()->handleSleepCall(m_thread->getId(), time_wake, start_time, end_time);

         if (m_thread->reschedule(end_time, core))
            core = m_thread->getCore();

         core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::SLEEP));

         if (rem)
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
         m_ret_val = success ? (Sim()->getConfig()->getApplicationCores()+7)/8 : -EINVAL;
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

      case -1:
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

      core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time_wake,
         m_syscall_number == SYS_pause ? SyncInstruction::PAUSE : SyncInstruction::SYSCALL));

      m_stalled = false;
   }

   if (!m_emulated)
   {
      m_ret_val = old_return;
   }

   Core *core = m_thread->getCore();
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
