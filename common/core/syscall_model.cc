#include "syscall_model.h"
#include "config.h"
#include "simulator.h"
#include "syscall_server.h"
#include "thread.h"
#include "core.h"
#include "core_manager.h"
#include "performance_model.h"
#include "pthread_emu.h"
#include "stats.h"

#include <errno.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "futex_emu.h"

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
      , m_ret_val(0)
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
}

SyscallMdl::~SyscallMdl()
{
   free(futex_counters);
}

void SyscallMdl::runEnter(IntPtr syscall_number, syscall_args_t &args)
{
   LOG_PRINT("Got Syscall: %i", syscall_number);

   m_syscall_number = syscall_number;

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
            SubsecondTime time = SubsecondTime::SEC(Sim()->getConfig()->getOSEmuTimeStart())
                               + m_thread->getCore()->getPerformanceModel()->getElapsedTime();

            switch(clock) {
               case CLOCK_REALTIME:
               case CLOCK_MONOTONIC:
                  ts->tv_sec = time.getNS() / 1000000000;
                  ts->tv_nsec = time.getNS() % 1000000000;
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

      case -1:
      default:
         break;
   }

   LOG_PRINT("Syscall finished");
}

IntPtr SyscallMdl::runExit(IntPtr old_return)
{
   if (m_emulated)
   {
      m_emulated = false;
      return m_ret_val;
   }
   else
   {
      return old_return;
   }
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
   LOG_ASSERT_ERROR(core != NULL, "Core should not be null");

   SubsecondTime start_time;
   SubsecondTime end_time;
   start_time = core->getPerformanceModel()->getElapsedTime();

   updateState(core, PthreadEmu::STATE_WAITING);

   IntPtr ret_val = Sim()->getSyscallServer()->handleFutexCall(m_thread->getId(), fargs, start_time, end_time);

   if (m_thread->reschedule(end_time, core))
      core = m_thread->getCore();

   core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(end_time, SyncInstruction::FUTEX));

   SubsecondTime delay = end_time - start_time;

   updateState(core, PthreadEmu::STATE_RUNNING, delay);

   // Update the futex statistics
   futexCount(cmd, delay);

   return ret_val;
}
