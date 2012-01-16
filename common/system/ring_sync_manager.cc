#include "ring_sync_client.h"
#include "ring_sync_manager.h"
#include "simulator.h"
#include "config.h"
#include "core_manager.h"
#include "packetize.h"
#include "message_types.h"
#include "utils.h"
#include "dvfs_manager.h"
#include "subsecond_time.h"
#include "config.hpp"

// There is one RingSyncManager per process
RingSyncManager::RingSyncManager():
   _transport(Transport::getSingleton()->getGlobalNode()),
   _slack(SubsecondTime::Zero())
{
   // Cache the core pointers corresponding to all the application cores
   UInt32 num_cores = Config::getSingleton()->getNumLocalCores();
   UInt32 num_app_cores = (Config::getSingleton()->getCurrentProcessNum() == 0) ? num_cores - 2 : num_cores - 1;

   Config::CoreList core_list = Config::getSingleton()->getCoreListForCurrentProcess();
   Config::CoreList::iterator it;

   for (it = core_list.begin(); it != core_list.end(); it++)
   {
      if ((*it) < (core_id_t) Sim()->getConfig()->getApplicationCores())
      {
         Core* core = Sim()->getCoreManager()->getCoreFromID(*it);
         assert(core != NULL);
         _core_list.push_back(core);
      }
   }
   assert(_core_list.size() == num_app_cores);

   // Has Fields
   try
   {
      _slack = SubsecondTime::NS() * Sim()->getCfg()->getInt("clock_skew_minimization/ring/slack");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Could not read 'clock_skew_minimization/ring/slack' from the config file");
   }
}

RingSyncManager::~RingSyncManager()
{}

void
RingSyncManager::generateSyncMsg()
{
   Config* config = Config::getSingleton();
   if (config->getCurrentProcessNum() == 0)
   {
      CycleCountUpdate cycle_count_update;
      cycle_count_update.min_elapsed_time = SubsecondTime::MaxTime();
      cycle_count_update.max_elapsed_time = _slack;

      UnstructuredBuffer send_msg;
      send_msg << LCP_MESSAGE_CLOCK_SKEW_MINIMIZATION;
      send_msg.put<CycleCountUpdate>(cycle_count_update);

      // Send it to process '0' itself
      _transport->globalSend(0, send_msg.getBuffer(), send_msg.size());
   }
}

void
RingSyncManager::processSyncMsg(Byte* msg)
{
   // How do I model the slight time delay that should be put in
   // before sending the message to another core
   CycleCountUpdate* cycle_count_update = (CycleCountUpdate*) msg;

   Config* config = Config::getSingleton();
   if (config->getCurrentProcessNum() == 0)
   {
      if (cycle_count_update->min_elapsed_time != SubsecondTime::MaxTime())
      {
         cycle_count_update->max_elapsed_time = cycle_count_update->min_elapsed_time + _slack;
         cycle_count_update->min_elapsed_time = SubsecondTime::MaxTime();
      }
   }

   // 1) Compute the min_cycle_count of all the cores in this process
   //    and update the message
   // 2) Update the max_cycle_count of all the cores
   // 3) Wake up any sleeping cores
   updateClientObjectsAndRingMsg(cycle_count_update);

   // I could easily prevent 1 copy here but I am just avoiding it now
   UnstructuredBuffer send_msg;
   send_msg << LCP_MESSAGE_CLOCK_SKEW_MINIMIZATION;
   send_msg.put<CycleCountUpdate>(*cycle_count_update);

   SInt32 next_process_num = (config->getCurrentProcessNum() + 1) % config->getProcessCount();
   _transport->globalSend(next_process_num, send_msg.getBuffer(), send_msg.size());
}

void
RingSyncManager::updateClientObjectsAndRingMsg(CycleCountUpdate* cycle_count_update)
{
   // Get the min cycle counts of all the application cores in this process
   SubsecondTime min_elapsed_time = SubsecondTime::MaxTime();

   std::vector<Core*>::iterator it;
   for (it = _core_list.begin(); it != _core_list.end(); it++)
   {
      // Read the Cycle Count and State of the core
      // May need locks around this
      Core::State core_state = (*it)->getState();

      RingSyncClient* ring_sync_client = (RingSyncClient*) (*it)->getClockSkewMinimizationClient();
      ring_sync_client->getLock()->acquire();

      SubsecondTime elapsed_time = ring_sync_client->getElapsedTime();
      if ((core_state == Core::RUNNING) && (elapsed_time < min_elapsed_time))
      {
         // Dont worry about the cycle counts of threads that are not running
         min_elapsed_time = elapsed_time;
      }

      // Update the max cycle count of each client
      ring_sync_client->setMaxElapsedTime(cycle_count_update->max_elapsed_time);

      ring_sync_client->getLock()->release();
   }

   cycle_count_update->min_elapsed_time = getMin<SubsecondTime>(cycle_count_update->min_elapsed_time, min_elapsed_time);
}
