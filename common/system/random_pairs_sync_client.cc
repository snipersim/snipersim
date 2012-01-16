#include "random_pairs_sync_client.h"
#include "simulator.h"
#include "config.h"
#include "packetize.h"
#include "network.h"
#include "log.h"
#include "dvfs_manager.h"
#include "subsecond_time.h"
#include "performance_model.h"
#include "config.hpp"

SubsecondTime RandomPairsSyncClient::MAX_ELAPSED_TIME = SubsecondTime::FS() << 60;

RandomPairsSyncClient::RandomPairsSyncClient(Core* core):
   _core(core),
   _last_sync_elapsed_time(SubsecondTime::Zero()),
   _quantum(NULL,0),
   _slack(NULL,0),
   _sleep_fraction(0.0)
{
   LOG_ASSERT_ERROR(Sim()->getConfig()->getApplicationCores() >= 3,
         "Number of Cores must be >= 3 if 'random_pairs' scheme is used");

   try
   {
      _slack = ComponentLatency(Sim()->getDvfsManager()->getCoreDomain(_core->getId()),Sim()->getCfg()->getInt("clock_skew_minimization/random_pairs/slack"));
      _quantum = ComponentLatency(Sim()->getDvfsManager()->getCoreDomain(_core->getId()), Sim()->getCfg()->getInt("clock_skew_minimization/random_pairs/quantum"));
      _sleep_fraction = Sim()->getCfg()->getFloat("clock_skew_minimization/random_pairs/sleep_fraction");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Could not read clock_skew_minimization/random_pairs variables from config file");
   }

   gettimeofday(&_start_wall_clock_time, NULL);
   _rand_num.seed(1);

   // Register Call-back
   _core->getNetwork()->registerCallback(CLOCK_SKEW_MINIMIZATION, ClockSkewMinimizationClientNetworkCallback, this);
}

RandomPairsSyncClient::~RandomPairsSyncClient()
{
   _core->getNetwork()->unregisterCallback(CLOCK_SKEW_MINIMIZATION);
}

void
RandomPairsSyncClient::enable()
{
   _enabled = true;

   assert(_msg_queue.empty());
   gettimeofday(&_start_wall_clock_time, NULL);
}

void
RandomPairsSyncClient::disable()
{
   _enabled = false;
}

// Called by network thread
void
RandomPairsSyncClient::netProcessSyncMsg(const NetPacket& recv_pkt)
{
   UInt32 msg_type;
   SubsecondTime elapsed_time;

   UnstructuredBuffer recv_buf;
   recv_buf << std::make_pair(recv_pkt.data, recv_pkt.length);

   recv_buf >> msg_type >> elapsed_time;
   SyncMsg sync_msg(recv_pkt.sender, (SyncMsg::MsgType) msg_type, elapsed_time);

   LOG_PRINT("Core(%i), SyncMsg[sender(%i), type(%u), time(%s)]",
         _core->getId(), sync_msg.sender, sync_msg.type, itostr(sync_msg.elapsed_time).c_str());

   LOG_ASSERT_ERROR(elapsed_time < MAX_ELAPSED_TIME,
         "SyncMsg[sender(%i), msg_type(%u), elapsed_time(%s)]",
         recv_pkt.sender, msg_type, itostr(elapsed_time).c_str());

   _lock.acquire();

   // SyncMsg
   //  - sender
   //  - type (REQ,ACK,WAIT)
   //  - elapsed_time
   // Called by the Network thread
   Core::State core_state = _core->getState();
   if (core_state == Core::RUNNING)
   {
      // Thread is RUNNING on core
      // Network thread must process the random sync requests
      if (sync_msg.type == SyncMsg::REQ)
      {
         // This may generate some WAIT messages
         processSyncReq(sync_msg, false);
      }
      else if (sync_msg.type == SyncMsg::ACK)
      {
         // sync_msg.type == SyncMsg::ACK with '0' or non-zero wait time
         _msg_queue.push_back(sync_msg);
         _cond.signal();
      }
      else
      {
         LOG_PRINT_ERROR("Unrecognized Sync Msg, type(%u) from(%i)", sync_msg.type, sync_msg.sender);
      }
   }
   else if (core_state == Core::SLEEPING)
   {
      LOG_ASSERT_ERROR(sync_msg.type == SyncMsg::REQ,
            "sync_msg.type(%u)", sync_msg.type);

      processSyncReq(sync_msg, true);
   }
   else
   {
      // I dont want to synchronize against a non-running core
      LOG_ASSERT_ERROR(sync_msg.type == SyncMsg::REQ,
            "sync_msg.type(%u)", sync_msg.type);

      LOG_PRINT("Core(%i) not RUNNING: Sending ACK", _core->getId());

      UnstructuredBuffer send_buf;
      send_buf << (UInt32) SyncMsg::ACK << SubsecondTime::Zero();
      _core->getNetwork()->netSend(sync_msg.sender, CLOCK_SKEW_MINIMIZATION, send_buf.getBuffer(), send_buf.size());
   }

   _lock.release();
}

void
RandomPairsSyncClient::processSyncReq(const SyncMsg& sync_msg, bool sleeping)
{
   assert(sync_msg.elapsed_time >= _slack.getLatency());

   // I dont want to lock this, so I just try to read the cycle count
   // Even if this is an approximate value, this is OK
   SubsecondTime elapsed_time = _core->getPerformanceModel()->getElapsedTime();

   LOG_ASSERT_ERROR(elapsed_time < MAX_ELAPSED_TIME, "Cycle Count(%s)", itostr(elapsed_time).c_str());

   LOG_PRINT("Core(%i): Time(%s), SyncReq[sender(%i), msg_type(%u), elapsed_time(%s)]",
      _core->getId(), itostr(elapsed_time).c_str(), sync_msg.sender, sync_msg.type, itostr(sync_msg.elapsed_time).c_str());

   // 3 possible scenarios
   if (elapsed_time > (sync_msg.elapsed_time + _slack.getLatency()))
   {
      // Wait till the other core reaches this one
      UnstructuredBuffer send_buf;
      send_buf << (UInt32) SyncMsg::ACK << SubsecondTime::Zero();
      _core->getNetwork()->netSend(sync_msg.sender, CLOCK_SKEW_MINIMIZATION, send_buf.getBuffer(), send_buf.size());

      if (!sleeping)
      {
         // Goto sleep for a few microseconds
         // Self generate a WAIT msg
         LOG_PRINT("Core(%i): WAIT: Time(%s)", _core->getId(), itostr(elapsed_time - sync_msg.elapsed_time).c_str());
         LOG_ASSERT_ERROR((elapsed_time - sync_msg.elapsed_time) < MAX_ELAPSED_TIME,
               "[>]: elapsed_time(%s), sync_msg[sender(%i), msg_type(%u), elapsed_time(%s)]",
               itostr(elapsed_time).c_str(), sync_msg.sender, sync_msg.type, itostr(sync_msg.elapsed_time).c_str());

         SyncMsg wait_msg(_core->getId(), SyncMsg::WAIT, elapsed_time - sync_msg.elapsed_time);
         _msg_queue.push_back(wait_msg);
      }
   }
   else if ((elapsed_time <= (sync_msg.elapsed_time + _slack.getLatency())) && (elapsed_time >= (sync_msg.elapsed_time - _slack.getLatency())))
   {
      // Both the cores are in sync (Good)
      UnstructuredBuffer send_buf;
      send_buf << (UInt32) SyncMsg::ACK << SubsecondTime::Zero();
      _core->getNetwork()->netSend(sync_msg.sender, CLOCK_SKEW_MINIMIZATION, send_buf.getBuffer(), send_buf.size());
   }
   else if (elapsed_time < (sync_msg.elapsed_time - _slack.getLatency()))
   {
      LOG_ASSERT_ERROR((sync_msg.elapsed_time - elapsed_time) < MAX_ELAPSED_TIME,
            "[<]: elapsed_time(%s), sync_msg[sender(%i), msg_type(%u), elapsed_time(%s)]",
            itostr(elapsed_time).c_str(), sync_msg.sender, sync_msg.type, itostr(sync_msg.elapsed_time).c_str());

      // Double up and catch up. Meanwhile, ask the other core to wait
      UnstructuredBuffer send_buf;
      send_buf << (UInt32) SyncMsg::ACK << (sync_msg.elapsed_time - elapsed_time);
      _core->getNetwork()->netSend(sync_msg.sender, CLOCK_SKEW_MINIMIZATION, send_buf.getBuffer(), send_buf.size());
   }
   else
   {
      LOG_PRINT_ERROR("Strange cycle counts: elapsed_time(%s), sender(%i), sender_elapsed_time(%s)",
            itostr(elapsed_time).c_str(), sync_msg.sender, itostr(sync_msg.elapsed_time).c_str());
   }
}

// Called by user thread
void
RandomPairsSyncClient::synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg)
{
   LOG_ASSERT_ERROR(time == SubsecondTime::Zero(), "Time(%s), Cannot be used", itostr(time).c_str());

   if (! _enabled)
      return;

   if (_core->getState() == Core::WAKING_UP)
      _core->setState(Core::RUNNING);

   SubsecondTime elapsed_time = _core->getPerformanceModel()->getElapsedTime();
   assert(elapsed_time >= _last_sync_elapsed_time);

   LOG_ASSERT_ERROR(elapsed_time < MAX_ELAPSED_TIME, "elapsed_time(%s)", itostr(elapsed_time).c_str());

   SubsecondTime quantum_latency = _quantum.getLatency();

   if ((elapsed_time - _last_sync_elapsed_time) >= quantum_latency)
   {
      LOG_PRINT("Core(%i): Starting Synchronization: elapsed_time(%s), LastSyncTime(%s)", _core->getId(), itostr(elapsed_time).c_str(), itostr(_last_sync_elapsed_time).c_str());

      _lock.acquire();

      _last_sync_elapsed_time = (elapsed_time / quantum_latency) * quantum_latency;

      LOG_ASSERT_ERROR(_last_sync_elapsed_time < MAX_ELAPSED_TIME, "_last_sync_elapsed_time(%s)", itostr(_last_sync_elapsed_time).c_str());

      // Send SyncMsg to another core
      sendRandomSyncMsg();

      // Wait for Acknowledgement and other Wait messages
      _cond.wait(_lock);

      SubsecondTime wait_time = userProcessSyncMsgList();

      LOG_PRINT("Wait Time (%s)", itostr(wait_time).c_str());

      gotoSleep(wait_time);

      _lock.release();

   }
}

void
RandomPairsSyncClient::sendRandomSyncMsg()
{
   SubsecondTime elapsed_time = _core->getPerformanceModel()->getElapsedTime();

   LOG_ASSERT_ERROR(elapsed_time < MAX_ELAPSED_TIME, "elapsed_time(%s)", itostr(elapsed_time).c_str());

   UInt32 num_app_cores = Config::getSingleton()->getApplicationCores();
   SInt32 offset = 1 + (SInt32) _rand_num.next((Random::value_t) (((float)num_app_cores - 1) / 2));
   core_id_t receiver = (_core->getId() + offset) % num_app_cores;

   LOG_ASSERT_ERROR((receiver >= 0) && (receiver < (core_id_t) num_app_cores),
         "receiver(%i)", receiver);

   LOG_PRINT("Core(%i) Sending SyncReq to %i", _core->getId(), receiver);

   UnstructuredBuffer send_buf;
   send_buf << (UInt32) SyncMsg::REQ << elapsed_time;
   _core->getNetwork()->netSend(receiver, CLOCK_SKEW_MINIMIZATION, send_buf.getBuffer(), send_buf.size());
}

SubsecondTime
RandomPairsSyncClient::userProcessSyncMsgList()
{
   bool ack_present = false;
   SubsecondTime max_wait_time = SubsecondTime::Zero();

   std::list<SyncMsg>::iterator it;
   for (it = _msg_queue.begin(); it != _msg_queue.end(); it++)
   {
      LOG_PRINT("Core(%i) Process Sync Msg List: SyncMsg[sender(%i), type(%u), wait_time(%s)]",
            _core->getId(), (*it).sender, (*it).type, itostr((*it).elapsed_time).c_str());

      LOG_ASSERT_ERROR((*it).elapsed_time < MAX_ELAPSED_TIME,
            "sync_msg[sender(%i), msg_type(%u), elapsed_time(%s)]",
            (*it).sender, (*it).type, itostr((*it).elapsed_time).c_str());

      assert(((*it).type == SyncMsg::WAIT) || ((*it).type == SyncMsg::ACK));

      if ((*it).elapsed_time >= max_wait_time)
         max_wait_time = (*it).elapsed_time;
      if ((*it).type == SyncMsg::ACK)
         ack_present = true;
   }

   assert(ack_present);
   _msg_queue.clear();

   return max_wait_time;
}

void
RandomPairsSyncClient::gotoSleep(const SubsecondTime sleep_time)
{
   LOG_ASSERT_ERROR(sleep_time < MAX_ELAPSED_TIME, "sleep_time(%s)", itostr(sleep_time).c_str());

   if (sleep_time > SubsecondTime::Zero())
   {
      LOG_PRINT("Core(%i) going to sleep", _core->getId());

      // Set the CoreState to 'SLEEPING'
      _core->setState(Core::SLEEPING);

      SubsecondTime elapsed_simulated_time = _core->getPerformanceModel()->getElapsedTime();
      SubsecondTime elapsed_wall_clock_time = SubsecondTime::US() * getElapsedWallClockTime();

      // elapsed_simulated_time, sleep_time - in cycles (of target architecture)
      // elapsed_wall_clock_time - in microseconds
      assert(elapsed_simulated_time != SubsecondTime::Zero());

      // This data is provided in the internal SubsecondTime format, femtoseconds.
      UInt64 elapsed_wall_clock_time_femto = elapsed_wall_clock_time.getInternalDataForced();
      UInt64 elapsed_simulated_time_femto = elapsed_simulated_time.getInternalDataForced();
      // Sleep time, in femtoseconds, convert to microseconds below
      UInt64 sleep_time_femto = sleep_time.getInternalDataForced();

      useconds_t sleep_wall_clock_time = (useconds_t) (_sleep_fraction * (elapsed_wall_clock_time_femto / elapsed_simulated_time_femto) * sleep_time_femto / 1000000000);
      if (sleep_wall_clock_time > 1000000)
      {
         LOG_PRINT_WARNING("Large Sleep Time(%u microseconds), SimSleep Time(%s), elapsed_wall_clock_time(%s), elapsed_simulated_time(%s)", sleep_wall_clock_time, itostr(sleep_time).c_str(), itostr(elapsed_wall_clock_time).c_str(), itostr(elapsed_simulated_time).c_str());
         sleep_wall_clock_time = 1000000;
      }

      _lock.release();

      assert(usleep(sleep_wall_clock_time) == 0);

      _lock.acquire();

      // Set the CoreState to 'RUNNING'
      _core->setState(Core::RUNNING);

      LOG_PRINT("Core(%i) woken up", _core->getId());
   }
}

UInt64
RandomPairsSyncClient::getElapsedWallClockTime()
{
   // Returns the elapsed time in microseconds
   struct timeval curr_wall_clock_time;
   gettimeofday(&curr_wall_clock_time, NULL);

   if (curr_wall_clock_time.tv_usec < _start_wall_clock_time.tv_usec)
   {
      curr_wall_clock_time.tv_usec += 1000000;
      curr_wall_clock_time.tv_sec -= 1;
   }
   return ( ((UInt64) (curr_wall_clock_time.tv_sec - _start_wall_clock_time.tv_sec)) * 1000000 +
      (UInt64) (curr_wall_clock_time.tv_usec - _start_wall_clock_time.tv_usec));
}
