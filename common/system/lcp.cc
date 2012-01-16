#include "lcp.h"
#include "simulator.h"
#include "core.h"
#include "message_types.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "dvfs_manager.h"
#include "clock_skew_minimization_object.h"
#include "stats.h"
#include "performance_model.h"

#include "log.h"
#include "subsecond_time.h"

// -- general LCP functionality

LCP::LCP()
   : m_proc_num(Config::getSingleton()->getCurrentProcessNum())
   , m_transport(Transport::getSingleton()->getGlobalNode())
   , m_finished(false)
{
}

LCP::~LCP()
{
}

void LCP::run()
{
   LOG_PRINT("LCP started.");

   while (!m_finished)
   {
      processPacket();
   }
}

void LCP::processPacket()
{
   Byte *pkt = m_transport->recv();

   SInt32 *msg_type = (SInt32*)pkt;

   LOG_PRINT("Received message type: %d", *msg_type);

   Byte *data = pkt + sizeof(SInt32);

   switch (*msg_type)
   {
   case LCP_MESSAGE_QUIT:
      LOG_PRINT("Received quit message.");
      m_finished = true;
      break;

   case LCP_MESSAGE_COMMID_UPDATE:
      updateCommId(data);
      break;

   case LCP_MESSAGE_SIMULATOR_FINISHED:
      Sim()->handleFinish();
      break;

   case LCP_MESSAGE_SIMULATOR_FINISHED_ACK:
      Sim()->deallocateProcess();
      break;

   case LCP_MESSAGE_THREAD_SPAWN_REQUEST_FROM_MASTER:
      Sim()->getThreadManager()->slaveSpawnThread((ThreadSpawnRequest*)pkt);
      break;

   case LCP_MESSAGE_QUIT_THREAD_SPAWNER:
      Sim()->getThreadManager()->slaveTerminateThreadSpawner();
      break;

   case LCP_MESSAGE_QUIT_THREAD_SPAWNER_ACK:
      Sim()->getThreadManager()->updateTerminateThreadSpawner();
      break;

   case LCP_MESSAGE_CLOCK_SKEW_MINIMIZATION:
      assert (Sim()->getClockSkewMinimizationManager());
      Sim()->getClockSkewMinimizationManager()->processSyncMsg(data);
      break;

   case LCP_MESSAGE_MAGIC:
      handleMagicMessage(reinterpret_cast<LCPMessageMagic*>(pkt));
      break;

   case LCP_MESSAGE_ACK:
      LOG_ASSERT_ERROR(Sim()->getConfig()->getCurrentProcessNum() == 0, "Got LCP_MESSAGE_ACK message as process %d", Sim()->getConfig()->getCurrentProcessNum());
      ackProcessAck();
      break;

   default:
      LOG_PRINT_ERROR("Unexpected LCP message type: %d.", *msg_type);
      break;
   }

   delete [] pkt;
}

void LCP::finish()
{
   LOG_PRINT("Send LCP quit message");

   SInt32 msg_type = LCP_MESSAGE_QUIT;

   m_transport->globalSend(m_proc_num,
                           &msg_type,
                           sizeof(msg_type));

   while (!m_finished)
      sched_yield();

   LOG_PRINT("LCP finished.");
}

void LCP::ackInit(UInt64 count)
{
   if (count == 0)
      m_ack_num = Sim()->getConfig()->getProcessCount();
   else
      m_ack_num = count;
}

void LCP::ackWait()
{
   // To be called by MCP, LCP thread should be allowed to run to collect LCP_MESSAGE_ACK messages
   while(m_ack_num > 0)
      sched_yield();
}

void LCP::ackProcessAck()
{
   --m_ack_num;
}

void LCP::ackSendAck()
{
   if (Sim()->getConfig()->getCurrentProcessNum() == 0)
      ackProcessAck();
   else {
      Transport::Node *globalNode = Transport::getSingleton()->getGlobalNode();
      SInt32 msg = LCP_MESSAGE_ACK;
      globalNode->globalSend(0, &msg, sizeof(msg));
   }
}


// -- functions for specific tasks

struct CommMapUpdate
{
   SInt32 comm_id;
   core_id_t core_id;
};

void LCP::updateCommId(void *vp)
{
   CommMapUpdate *update = (CommMapUpdate*)vp;

   LOG_PRINT("Initializing comm_id: %d to core_id: %d", update->comm_id, update->core_id);
   Config::getSingleton()->updateCommToCoreMap(update->comm_id, update->core_id);

   NetPacket ack(/*time*/ SubsecondTime::Zero(),
                 /*type*/ LCP_COMM_ID_UPDATE_REPLY,
                 /*sender*/ 0, // doesn't matter ; see core_manager.cc
                 /*receiver*/ update->core_id,
                 /*length*/ 0,
                 /*data*/ NULL);
   Byte *buffer = ack.makeBuffer();
   m_transport->send(update->core_id, buffer, ack.bufferSize());
   delete [] buffer;
}

static Timer t_start;
UInt64 ninstrs_start;
__attribute__((weak)) void PinDetach(void) {}

UInt64 getGlobalInstructionCount(void)
{
   UInt64 ninstrs = 0;
   for (UInt32 i = 0; i < Sim()->getConfig()->getNumLocalCores(); i++)
      ninstrs += Sim()->getCoreManager()->getCoreFromIndex(i)->getInstructionCount();
   return ninstrs;
}

void LCP::enablePerformance()
{
   Sim()->getStatsManager()->recordStats("roi-begin");
   ninstrs_start = getGlobalInstructionCount();
   t_start.start();

   Simulator::enablePerformanceModelsInCurrentProcess();
   Sim()->setInstrumentationMode(InstMode::inst_mode_roi);
   ackSendAck();
}

void LCP::disablePerformance()
{
   Simulator::disablePerformanceModelsInCurrentProcess();
   Sim()->getStatsManager()->recordStats("roi-end");

   float seconds = t_start.getTime() / 1e9;
   UInt64 ninstrs = getGlobalInstructionCount() - ninstrs_start;
   printf("[SNIPER:%u] Simulated %.1fM instructions @ %.1f KIPS (%.1f KIPS / target core - %.1fns/instr)\n",
      Sim()->getConfig()->getCurrentProcessNum(),
      ninstrs / 1e6, ninstrs / seconds / 1e3,
      ninstrs / seconds / 1e3 / Sim()->getConfig()->getNumLocalApplicationCores(),
      seconds * 1e9 / (float(ninstrs ? ninstrs : 1.) / Sim()->getConfig()->getNumLocalApplicationCores()));
   fflush(NULL);

   Sim()->setInstrumentationMode(InstMode::inst_mode_end);
   PinDetach();
   ackSendAck();
}

void LCP::setFrequency(UInt64 core_number, UInt64 freq_in_hz)
{
   Sim()->getDvfsManager()->setCoreDomain(core_number, ComponentPeriod::fromFreqHz(freq_in_hz));
   ackSendAck();
}

void LCP::setInstrumentationMode(InstMode::inst_mode_t inst_mode)
{
   Sim()->setInstrumentationMode(inst_mode);
   ackSendAck();
}

void LCP::handleMagicMessage(LCPMessageMagic *msg)
{
   switch (msg->cmd)
   {
   case LCP_MAGIC_CMD_PERFORMANCE_ENABLE:
      enablePerformance();
      break;
   case LCP_MAGIC_CMD_PERFORMANCE_DISABLE:
      disablePerformance();
      break;
   case LCP_MAGIC_CMD_SET_FREQUENCY:
      setFrequency(msg->arg0, msg->arg1);
      break;
   case LCP_MAGIC_CMD_KILL_THREAD:
      Sim()->getCoreManager()->getCoreFromID(msg->arg0)->setState(Core::BROKEN);
      break;
   case LCP_MAGIC_CMD_INSTRUMENT_MODE:
      InstMode::inst_mode_t inst_mode;
      switch (msg->arg0)
      {
      case LCP_MAGIC_OPT_INSTRUMENT_DETAILED:
         inst_mode = InstMode::DETAILED;
         break;
      case LCP_MAGIC_OPT_INSTRUMENT_WARMUP:
         inst_mode = InstMode::CACHE_ONLY;
         break;
      case LCP_MAGIC_OPT_INSTRUMENT_FASTFORWARD:
         inst_mode = InstMode::FAST_FORWARD;
         break;
      default:
         LOG_PRINT_ERROR("Unexpected LCP magic instrument opt type: %lx.", msg->cmd);
      }
      setInstrumentationMode(inst_mode);
      break;
   default:
      LOG_PRINT_ERROR("Unexpected LCP magic cmd message type: %lx.", msg->cmd);
      break;
   }
}
