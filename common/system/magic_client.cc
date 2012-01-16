#include "magic_client.h"
#include "sim_api.h"
#include "mcp.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"

UInt64 sendMagicToMCP(UInt64 cmd, UInt64 arg0 = 0, UInt64 arg1 = 0);

void enablePerformanceGlobal(void)
{
   sendMagicToMCP(SIM_CMD_ROI_START);
}

void disablePerformanceGlobal(void)
{
   sendMagicToMCP(SIM_CMD_ROI_END);
}

void setInstrumentationMode(UInt64 opt)
{
   sendMagicToMCP(SIM_CMD_INSTRUMENT_MODE, opt);
}

bool ignoreMagicType(UInt64 cmd)
{
   // Magic instruction commands to ignore when --no-roi is used
   switch(cmd) {
      case SIM_CMD_ROI_TOGGLE:
      case SIM_CMD_ROI_START:
      case SIM_CMD_ROI_END:
         return true;
      default:
         return false;
   }
}

UInt64 sendMagicToMCP(UInt64 cmd, UInt64 arg0, UInt64 arg1)
{
   UnstructuredBuffer m_send_buff;
   int msg_type = MCP_MESSAGE_MAGIC;
   m_send_buff << msg_type << cmd << arg0 << arg1;

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   if (!core) {
      // If we're not being called in Core context, use the first local core's network
      core_id_t core_id = Sim()->getConfig()->getCoreListForCurrentProcess().at(0);
      core = Sim()->getCoreManager()->getCoreFromID(core_id);
   }
   Network *net = core->getNetwork();
   net->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   UInt64 res = 0;

   // INSTRUMENT_MODE is asynchronous
   // This is done because perf-events' signals hold the VmLock in Pin, but PIN_RemoveInstrumentation also requires that lock.
   // This is not necessarily needed for non-signal requests
   if (cmd != SIM_CMD_INSTRUMENT_MODE)
   {
      NetPacket recv_pkt;
      recv_pkt = net->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

      assert(recv_pkt.length == sizeof(UInt64));
      res = *(UInt64 *)recv_pkt.data;
      delete [](Byte*) recv_pkt.data;
   }

   return res;
}

UInt64 handleMagicInstruction(UInt64 cmd, UInt64 arg0, UInt64 arg1)
{
   // when --general/magic=false, ignore ROI begin/end magic instructions
   if (!Sim()->getConfig()->useMagic() && ignoreMagicType(cmd))
      return -1;

   switch(cmd)
   {
   case SIM_CMD_ROI_TOGGLE:
   case SIM_CMD_ROI_START:
   case SIM_CMD_ROI_END:
   case SIM_CMD_MHZ_SET:
   case SIM_CMD_MARKER:
   case SIM_CMD_USER:
   case SIM_CMD_INSTRUMENT_MODE:
   case SIM_CMD_MHZ_GET:
      return sendMagicToMCP(cmd, arg0, arg1);
   case SIM_CMD_IN_SIMULATOR:
      return 0;
   default:
      LOG_PRINT_WARNING_ONCE("Encountered unknown magic instruction cmd(%u)", cmd);
      return 1;
   }
}
