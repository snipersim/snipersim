#include "magic_server.h"
#include "sim_api.h"
#include "simulator.h"
#include "lcp.h"
#include "thread_manager.h"
#include "logmem.h"
#include "core_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "timer.h"

MagicServer::MagicServer(Network &network, UnstructuredBuffer &recv_buffer)
      : m_network(network)
      , m_recv_buffer(recv_buffer)
      , m_performance_enabled(false)
      , m_instrument_mode((LCPMagicOptTypes)-1)
{
}

MagicServer::~MagicServer()
{ }

void MagicServer::handlePacket(core_id_t core_id)
{
   UInt64 cmd, arg0, arg1, res;
   m_recv_buffer >> cmd;
   m_recv_buffer >> arg0;
   m_recv_buffer >> arg1;

   res = Magic(core_id, cmd, arg0, arg1);

   // INSTRUMENT_MODE is asynchronous
   if (cmd != SIM_CMD_INSTRUMENT_MODE)
      m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&res, sizeof(res));
}

UInt64 MagicServer::Magic(core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1)
{
   switch(cmd)
   {
      case SIM_CMD_ROI_TOGGLE:
         return setPerformance(! m_performance_enabled);
      case SIM_CMD_ROI_START:
         if (! m_performance_enabled)
            Sim()->getHooksManager()->callHooks(HookType::HOOK_ROI_BEGIN, 0);
         return setPerformance(true);
      case SIM_CMD_ROI_END:
         if (m_performance_enabled)
            Sim()->getHooksManager()->callHooks(HookType::HOOK_ROI_END, 0);
         return setPerformance(false);
      case SIM_CMD_MHZ_SET:
         return setFrequency(arg0 == UINT64_MAX ? core_id : arg0, arg1);
      case SIM_CMD_MARKER:
      {
         MagicMarkerType args = { core_id: core_id, arg0: arg0, arg1: arg1 };
         Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_MARKER, &args);
         return 0;
      }
      case SIM_CMD_USER:
      {
         MagicMarkerType args = { core_id: core_id, arg0: arg0, arg1: arg1 };
         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, &args, true /* expect return value */);
      }
      case SIM_CMD_INSTRUMENT_MODE:
         return setInstrumentationMode(arg0);
      case SIM_CMD_MHZ_GET:
         return getFrequency(arg0 == UINT64_MAX ? core_id : arg0);
      default:
         LOG_ASSERT_ERROR(false, "Got invalid Magic %lu, arg0(%lu) arg1(%lu)", cmd, arg0, arg1);
   }
   return 0;
}

void print_allocations();

UInt64 MagicServer::setPerformance(bool enabled)
{
   if (m_performance_enabled == enabled)
      return 1;

   m_performance_enabled = enabled;
   m_instrument_mode = LCP_MAGIC_OPT_INSTRUMENT_DETAILED;

   //static bool enabled = false;
   static Timer t_start;
   //ScopedLock sl(l_alloc);

   if (m_performance_enabled) {
      printf("[SNIPER] Enabling performance models\n");
      fflush(NULL);
      t_start.start();
      logmem_enable(true);
   } else {
      printf("[SNIPER] Disabling performance models\n");
      float seconds = t_start.getTime() / 1e9;
      printf("[SNIPER] Leaving ROI after %.2f seconds\n", seconds);
      fflush(NULL);
      logmem_enable(false);
      logmem_write_allocations();
   }

   // LCP (proc 0 == MCP) clear the number of LCP responders
   Sim()->getLCP()->ackInit();

   Transport::Node *node = Transport::getSingleton()->getGlobalNode();
   for(UInt32 pid = 0; pid < Config::getSingleton()->getProcessCount(); pid++)
   {
      LCPMessageMagic msg;
      msg.lcp_cmd = LCP_MESSAGE_MAGIC;
      msg.cmd = enabled ? LCP_MAGIC_CMD_PERFORMANCE_ENABLE : LCP_MAGIC_CMD_PERFORMANCE_DISABLE;
      msg.arg0 = msg.arg1 = 0;
      LOG_PRINT("Sending magic performance message to process %d", pid);
      node->globalSend(pid, &msg, sizeof (msg));
   }

   // LCP (proc 0 == MCP) wait for n LCP responses
   Sim()->getLCP()->ackWait();

   return 0;
}

UInt64 MagicServer::setFrequency(UInt64 core_number, UInt64 freq_in_mhz)
{
   UInt32 num_cores = Sim()->getConfig()->getApplicationCores();
   UInt64 freq_in_hz;
   if (core_number >= num_cores)
      return 1;
   freq_in_hz = 1000000 * freq_in_mhz;

   printf("[SNIPER] Setting frequency for core %ld in DVFS domain %d to %ld MHz\n", core_number, Sim()->getDvfsManager()->getCoreDomainId(core_number), freq_in_mhz);

   // Update process 0's copy of all frequencies
   if (freq_in_hz > 0)
      Sim()->getDvfsManager()->setCoreDomain(core_number, ComponentPeriod::fromFreqHz(freq_in_hz));
   else {
      Sim()->getThreadManager()->stallThread(core_number, SubsecondTime::MaxTime());
      Sim()->getCoreManager()->getCoreFromID(core_number)->setState(Core::BROKEN);
   }

   // Update process that has this core (if different from process 0)

   // Find the process number that contains the core that we would like to update
   UInt32 process_num = Sim()->getConfig()->getProcessNumForCore(core_number);
   if (process_num != Sim()->getConfig()->getCurrentProcessNum()) {
      // Only wait for 1 process: the LCP that owns this CPU
      Sim()->getLCP()->ackInit(1);

      LCPMessageMagic msg;
      msg.lcp_cmd = LCP_MESSAGE_MAGIC;
      msg.cmd = freq_in_hz > 0 ? LCP_MAGIC_CMD_SET_FREQUENCY : LCP_MAGIC_CMD_KILL_THREAD;
      msg.arg0 = core_number;
      msg.arg1 = freq_in_hz;

      LOG_PRINT("Sending magic mhz message to process %d", process_num);
      Transport::Node *node = Transport::getSingleton()->getGlobalNode();
      node->globalSend(process_num, &msg, sizeof (msg));

      // Wait for the one process
      Sim()->getLCP()->ackWait();
   }

   // First set frequency, then call hooks so hook script can find the new frequency by querying the DVFS manager
   Sim()->getHooksManager()->callHooks(HookType::HOOK_CPUFREQ_CHANGE, (void*)core_number);

   return 0;
}

UInt64 MagicServer::getFrequency(UInt64 core_number)
{
   UInt32 num_cores = Sim()->getConfig()->getApplicationCores();
   if (core_number >= num_cores)
      return UINT64_MAX;

   const ComponentPeriod *per = Sim()->getDvfsManager()->getCoreDomain(core_number);
   return per->getPeriodInFreqMHz();
}

UInt64 MagicServer::setInstrumentationMode(UInt64 sim_api_opt)
{
   LOG_ASSERT_ERROR(Sim()->getCoreManager()->getCurrentCoreID() == Sim()->getConfig()->getMCPCoreNum(),
      "setInstrumentationMode must be run from MCP context");

   LCPMagicOptTypes lcp_instrument_opt;

   switch (sim_api_opt) {
      case SIM_OPT_INSTRUMENT_DETAILED:
         lcp_instrument_opt = LCP_MAGIC_OPT_INSTRUMENT_DETAILED;
         break;
      case SIM_OPT_INSTRUMENT_WARMUP:
         lcp_instrument_opt = LCP_MAGIC_OPT_INSTRUMENT_WARMUP;
         break;
      case SIM_OPT_INSTRUMENT_FASTFORWARD:
         lcp_instrument_opt = LCP_MAGIC_OPT_INSTRUMENT_FASTFORWARD;
         break;
      default:
         LOG_PRINT_ERROR("Got invalid setInstrumentOption arg0(%lu)", sim_api_opt);
   }

   // FIXME InstMode also keeps track of the current instrumentation mode
   if (m_instrument_mode == lcp_instrument_opt)
      return 1;

   m_instrument_mode = lcp_instrument_opt;

   // Do not print the mode change, because InstMode prints those changes already

   // LCP (proc 0 == MCP) clear the number of LCP responders
   Sim()->getLCP()->ackInit();

   Transport::Node *node = Transport::getSingleton()->getGlobalNode();
   for(UInt32 pid = 0; pid < Config::getSingleton()->getProcessCount(); pid++)
   {
      LCPMessageMagic msg;
      msg.lcp_cmd = LCP_MESSAGE_MAGIC;
      msg.cmd = LCP_MAGIC_CMD_INSTRUMENT_MODE;
      msg.arg0 = lcp_instrument_opt;
      msg.arg1 = 0;
      LOG_PRINT("Sending magic instrumentation mode to process %d", pid);
      node->globalSend(pid, &msg, sizeof (msg));
   }

   // LCP (proc 0 == MCP) wait for n LCP responses
   Sim()->getLCP()->ackWait();

   Sim()->getHooksManager()->callHooks(HookType::HOOK_INSTRUMENT_MODE, (void*)sim_api_opt);

   return 0;
}
