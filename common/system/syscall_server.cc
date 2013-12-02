#include "syscall_server.h"
#include "core.h"
#include "config.h"
#include "config.hpp"
#include "simulator.h"
#include "hooks_manager.h"
#include "thread_manager.h"
#include "thread.h"
#include "core_manager.h"
#include "log.h"
#include "circular_log.h"

#include <sys/syscall.h>
#include "os_compat.h"

SyscallServer::SyscallServer()
{
   m_reschedule_cost = SubsecondTime::NS() * Sim()->getCfg()->getInt("perf_model/sync/reschedule_cost");

   Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, hook_periodic, (UInt64)this);
}

SyscallServer::~SyscallServer()
{
}

void SyscallServer::handleSleepCall(thread_id_t thread_id, SubsecondTime wake_time, SubsecondTime curr_time, SubsecondTime &end_time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());

   m_sleeping.push_back(SimFutex::Waiter(thread_id, 0, wake_time));
   end_time = Sim()->getThreadManager()->stallThread(thread_id, ThreadManager::STALL_SLEEP, curr_time);
}

IntPtr SyscallServer::handleFutexCall(thread_id_t thread_id, futex_args_t &args, SubsecondTime curr_time, SubsecondTime &end_time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   CLOG("futex", "Futex enter thread %d", thread_id);

   int cmd = (args.op & FUTEX_CMD_MASK) & ~FUTEX_PRIVATE_FLAG;
   SubsecondTime timeout_time = SubsecondTime::MaxTime();
   LOG_PRINT("Futex syscall: uaddr(0x%x), op(%u), val(%u)", args.uaddr, args.op, args.val);

   Core* core = Sim()->getThreadManager()->getThreadFromID(thread_id)->getCore();
   LOG_ASSERT_ERROR (core != NULL, "Core should not be NULL");

   // Right now, we handle only a subset of the functionality
   // assert the subset
   LOG_ASSERT_ERROR((cmd == FUTEX_WAIT) \
            || (cmd == FUTEX_WAIT_BITSET) \
            || (cmd == FUTEX_WAKE) \
            || (cmd == FUTEX_WAKE_BITSET) \
            || (cmd == FUTEX_WAKE_OP) \
            || (cmd == FUTEX_CMP_REQUEUE) \
            , "op = 0x%x", args.op);
   if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET)
   {
      if (args.timeout != NULL)
      {
         struct timespec timeout_buf;
         core->accessMemory(Core::NONE, Core::READ, (IntPtr) args.timeout, (char*) &timeout_buf, sizeof(timeout_buf));
         SubsecondTime timeout_val = SubsecondTime::SEC(timeout_buf.tv_sec - Sim()->getConfig()->getOSEmuTimeStart())
                                   + SubsecondTime::NS(timeout_buf.tv_nsec);
         if (cmd == FUTEX_WAIT_BITSET)
         {
            timeout_time = timeout_val;               // FUTEX_WAIT_BITSET uses absolute timeout
            LOG_ASSERT_WARNING_ONCE(timeout_val < curr_time + SubsecondTime::SEC(10), "FUTEX_WAIT_BITSET timeout value is more than 10 seconds in the future");
            LOG_ASSERT_WARNING_ONCE(timeout_val > curr_time, "FUTEX_WAIT_BITSET timeout value is in the past");
         }
         else
         {
            timeout_time = curr_time + timeout_val;   // FUTEX_WAIT uses relative timeout
         }
      }
   }

   int act_val;
   IntPtr res;

   core->accessMemory(Core::NONE, Core::READ, (IntPtr) args.uaddr, (char*) &act_val, sizeof(act_val));

   if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET)
   {
      res = futexWait(thread_id, args.uaddr, args.val, act_val, cmd == FUTEX_WAIT_BITSET ? args.val3 : FUTEX_BITSET_MATCH_ANY, curr_time, timeout_time, end_time);
   }
   else if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET)
   {
      res = futexWake(thread_id, args.uaddr, args.val, cmd == FUTEX_WAIT_BITSET ? args.val3 : FUTEX_BITSET_MATCH_ANY, curr_time, end_time);
   }
   else if (cmd == FUTEX_WAKE_OP)
   {
      res = futexWakeOp(thread_id, args.uaddr, args.uaddr2, args.val, args.val2, args.val3, curr_time, end_time);
   }
   else if(cmd == FUTEX_CMP_REQUEUE)
   {
      LOG_ASSERT_ERROR(args.uaddr != args.uaddr2, "FUTEX_CMP_REQUEUE: uaddr == uaddr2 == %p", args.uaddr);
      res = futexCmpRequeue(thread_id, args.uaddr, args.val, args.uaddr2, args.val3, act_val, curr_time, end_time);
   }
   else
   {
      LOG_PRINT_ERROR("Unknown SYS_futex cmd %d", cmd);
   }

   CLOG("futex", "Futex leave thread %d", thread_id);
   return res;
}


SubsecondTime SyscallServer::applyRescheduleCost(thread_id_t thread_id, bool conditional)
{
   if (conditional == false)
      return SubsecondTime::Zero();
   else if (Sim()->getThreadManager()->getThreadFromID(thread_id)->getCore()->getInstructionCount() == 0)
      // Do not apply reschedule cost when we have not yet executed any instructions.
      // This would be the case in MPI mode before ROI.
      return SubsecondTime::Zero();
   else
      return m_reschedule_cost;
}


// -- Futex related functions --
SimFutex* SyscallServer::findFutexByUaddr(int *uaddr, thread_id_t thread_id)
{
   // Assumes that for multi-programmed and private futexes, va2pa() still returns unique addresses for each thread
   IntPtr address = Sim()->getThreadManager()->getThreadFromID(thread_id)->va2pa((IntPtr)uaddr);
   SimFutex *sim_futex = &m_futexes[address];
   return sim_futex;
}

IntPtr SyscallServer::futexWait(thread_id_t thread_id, int *uaddr, int val, int act_val, int mask, SubsecondTime curr_time, SubsecondTime timeout_time, SubsecondTime &end_time)
{
   LOG_PRINT("Futex Wait");
   SimFutex *sim_futex = findFutexByUaddr(uaddr, thread_id);

   if (val != act_val)
   {
      end_time = curr_time;
      return -EWOULDBLOCK;
   }
   else
   {
      bool success = sim_futex->enqueueWaiter(thread_id, mask, curr_time, timeout_time, end_time);
      if (success)
         return 0;
      else
         return -ETIMEDOUT;
   }
}

thread_id_t SyscallServer::wakeFutexOne(SimFutex *sim_futex, thread_id_t thread_by, int mask, SubsecondTime curr_time)
{
   thread_id_t waiter = sim_futex->dequeueWaiter(thread_by, mask, curr_time + applyRescheduleCost(thread_by));
   return waiter;
}

IntPtr SyscallServer::futexWake(thread_id_t thread_id, int *uaddr, int nr_wake, int mask, SubsecondTime curr_time, SubsecondTime &end_time)
{
   LOG_PRINT("Futex Wake");
   SimFutex *sim_futex = findFutexByUaddr(uaddr, thread_id);
   int num_procs_woken_up = 0;

   for (int i = 0; i < nr_wake; i++)
   {
      thread_id_t waiter = wakeFutexOne(sim_futex, thread_id, mask, curr_time);
      if (waiter == INVALID_THREAD_ID)
         break;

      num_procs_woken_up ++;
   }

   end_time = curr_time + applyRescheduleCost(thread_id, num_procs_woken_up > 0);
   return num_procs_woken_up;
}

int SyscallServer::futexDoOp(Core *core, int encoded_op, int *uaddr)
{
   int op = (encoded_op >> 28) & 7;
   int cmp = (encoded_op >> 24) & 15;
   int oparg = (encoded_op << 8) >> 20;
   int cmparg = (encoded_op << 20) >> 20;
   int oldval, newval;
   int ret = 0;

   if (encoded_op & (FUTEX_OP_OPARG_SHIFT << 28))
      oparg = 1 << oparg;

   core->accessMemory(Core::LOCK, Core::READ_EX, (IntPtr) uaddr, (char*) &oldval, sizeof(oldval));

   switch (op) {
      case FUTEX_OP_SET:
         newval = oparg;
         break;
     case FUTEX_OP_ADD:
         newval = oldval + oparg;
         break;
     case FUTEX_OP_OR:
        newval = oldval | oparg;
        break;
     case FUTEX_OP_ANDN:
        newval = oldval & ~oparg;
        break;
     case FUTEX_OP_XOR:
        newval = oldval ^ oparg;
        break;
     default:
        LOG_ASSERT_ERROR(false, "futexWakeOp: unknown op = %d", op);
   }

   if (op != FUTEX_OP_SET)
   {
      // TODO: Implement these using an atomic read-modify-write version of core->accessMemory
      LOG_PRINT_WARNING_ONCE("FUTEX_WAKE_OP is implemented non-atomically, race condition may have occured");
   }

   core->accessMemory(Core::UNLOCK, Core::WRITE, (IntPtr) uaddr, (char*) &newval, sizeof(newval));

   switch (cmp) {
      case FUTEX_OP_CMP_EQ:
         ret = (oldval == cmparg);
         break;
      case FUTEX_OP_CMP_NE:
         ret = (oldval != cmparg);
         break;
      case FUTEX_OP_CMP_LT:
         ret = (oldval < cmparg);
         break;
      case FUTEX_OP_CMP_GE:
         ret = (oldval >= cmparg);
         break;
      case FUTEX_OP_CMP_LE:
         ret = (oldval <= cmparg);
         break;
      case FUTEX_OP_CMP_GT:
         ret = (oldval > cmparg);
         break;
      default:
        LOG_ASSERT_ERROR(false, "futexWakeOp: unknown cmp = %d", cmp);
   }

   return ret;
}

IntPtr SyscallServer::futexWakeOp(thread_id_t thread_id, int *uaddr, int *uaddr2, int nr_wake, int nr_wake2, int op, SubsecondTime curr_time, SubsecondTime &end_time)
{
   LOG_PRINT("Futex WakeOp");
   SimFutex *sim_futex = findFutexByUaddr(uaddr, thread_id);
   SimFutex *sim_futex2 = findFutexByUaddr(uaddr2, thread_id);
   int num_procs_woken_up = 0;

   Thread *thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
   Core *core = thread->getCore();
   LOG_ASSERT_ERROR(core != NULL, "Cannot execute futexWakeOp() for a thread that is unscheduled");
   int op_ret = futexDoOp(core, op, uaddr2);

   for (int i = 0; i < nr_wake; i++)
   {
      thread_id_t waiter = wakeFutexOne(sim_futex, thread_id, FUTEX_BITSET_MATCH_ANY, curr_time);
      if (waiter == INVALID_THREAD_ID)
         break;

      num_procs_woken_up ++;
   }

   if (op_ret > 0) {
      for (int i = 0; i < nr_wake2; i++)
      {
         thread_id_t waiter = wakeFutexOne(sim_futex2, thread_id, FUTEX_BITSET_MATCH_ANY, curr_time);
         if (waiter == INVALID_THREAD_ID)
            break;

         num_procs_woken_up ++;
      }
   }

   end_time = curr_time + applyRescheduleCost(thread_id, num_procs_woken_up > 0);
   return num_procs_woken_up;
}

IntPtr SyscallServer::futexCmpRequeue(thread_id_t thread_id, int *uaddr, int val, int *uaddr2, int val3, int act_val, SubsecondTime curr_time, SubsecondTime &end_time)
{
   LOG_PRINT("Futex CMP_REQUEUE");
   SimFutex *sim_futex = findFutexByUaddr(uaddr, thread_id);
   int num_procs_woken_up = 0;

   if(val3 != act_val)
   {
      end_time = curr_time;
      return -EAGAIN;
   }
   else
   {
      for(int i = 0; i < val; i++)
      {
         thread_id_t waiter = sim_futex->dequeueWaiter(thread_id, FUTEX_BITSET_MATCH_ANY, curr_time);
         if(waiter == INVALID_THREAD_ID)
            break;

         num_procs_woken_up++;
      }

      SimFutex *requeue_futex = findFutexByUaddr(uaddr2, thread_id);

      while(true)
      {
         // dequeueWaiter changes the thread state to
         // RUNNING, which is changed back to STALLED
         // by enqueueWaiter. Since only the MCP uses this state
         // this should be okay.
         thread_id_t waiter = sim_futex->requeueWaiter(requeue_futex);
         if(waiter == INVALID_THREAD_ID)
            break;
      }

      end_time = curr_time;
      return num_procs_woken_up;
   }
}

void SyscallServer::futexPeriodic(SubsecondTime time)
{
   // Wake sleeping threads
   for(SimFutex::ThreadQueue::iterator it = m_sleeping.begin(); it != m_sleeping.end(); ++it)
   {
      if (it->timeout <= time)
      {
         thread_id_t waiter = it->thread_id;
         m_sleeping.erase(it);

         Sim()->getThreadManager()->resumeThread(waiter, waiter, time, (void*)false);

         // Iterator will be invalid, wake up potential others in the next barrier synchronization which should only be 100ns away
         break;
      }
   }
   // Wake timeout futexes
   for(FutexMap::iterator it = m_futexes.begin(); it != m_futexes.end(); ++it)
   {
      it->second.wakeTimedOut(time);
   }
}

SubsecondTime SyscallServer::getNextTimeout(SubsecondTime time)
{
   SubsecondTime next = SubsecondTime::MaxTime();
   // Sleeping threads
   for(SimFutex::ThreadQueue::iterator it = m_sleeping.begin(); it != m_sleeping.end(); ++it)
   {
      if (it->timeout < SubsecondTime::MaxTime())
         next = it->timeout;
   }
   // Timeout futexes
   for(FutexMap::iterator it = m_futexes.begin(); it != m_futexes.end(); ++it)
   {
      SubsecondTime t = it->second.getNextTimeout(time);
      if (t < next)
         next = t;
   }
   return next;
}

// -- SimFutex -- //
SimFutex::SimFutex()
{}

SimFutex::~SimFutex()
{
   #if 0 // Disabled: applications are not required to do proper cleanup
   if (!m_waiting.empty())
   {
      printf("Threads still waiting for futex %p: ", this);
      while(!m_waiting.empty())
      {
         printf("%u ", m_waiting.front().thread_id);
         m_waiting.pop_front();
      }
      printf("\n");
   }
   #endif
}

bool SimFutex::enqueueWaiter(thread_id_t thread_id, int mask, SubsecondTime time, SubsecondTime timeout_time, SubsecondTime &time_end)
{
   m_waiting.push_back(Waiter(thread_id, mask, timeout_time));
   time_end = Sim()->getThreadManager()->stallThread(thread_id, ThreadManager::STALL_FUTEX, time);
   return Sim()->getThreadManager()->getThreadFromID(thread_id)->getWakeupMsg();
}

thread_id_t SimFutex::dequeueWaiter(thread_id_t thread_by, int mask, SubsecondTime time)
{
   if (m_waiting.empty())
      return INVALID_THREAD_ID;
   else
   {
      for(ThreadQueue::iterator it = m_waiting.begin(); it != m_waiting.end(); ++it)
      {
         if (mask & it->mask)
         {
            thread_id_t waiter = it->thread_id;
            m_waiting.erase(it);

            Sim()->getThreadManager()->resumeThread(waiter, thread_by, time, (void*)true);
            return waiter;
         }
      }
      return INVALID_THREAD_ID;
   }
}

thread_id_t SimFutex::requeueWaiter(SimFutex *requeue_futex)
{
   if (m_waiting.empty())
      return INVALID_THREAD_ID;
   else
   {
      Waiter waiter = m_waiting.front();
      m_waiting.pop_front();
      requeue_futex->m_waiting.push_back(waiter);

      return waiter.thread_id;
   }
}

void SimFutex::wakeTimedOut(SubsecondTime time)
{
   for(ThreadQueue::iterator it = m_waiting.begin(); it != m_waiting.end(); ++it)
   {
      if (it->timeout <= time)
      {
         thread_id_t waiter = it->thread_id;
         m_waiting.erase(it);

         Sim()->getThreadManager()->resumeThread(waiter, INVALID_THREAD_ID, time, (void*)false);

         // Iterator will be invalid, wake up potential others in the next barrier synchronization which should only be 100ns away
         break;
      }
   }
}

SubsecondTime SimFutex::getNextTimeout(SubsecondTime time)
{
   SubsecondTime next = SubsecondTime::MaxTime();
   for(ThreadQueue::iterator it = m_waiting.begin(); it != m_waiting.end(); ++it)
   {
      if (it->timeout < SubsecondTime::MaxTime())
         next = it->timeout;
   }
   return next;
}
