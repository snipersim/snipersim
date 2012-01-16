#ifndef MAGIC_SERVER_H
#define MAGIC_SERVER_H

#include "network.h"
#include "packetize.h"
#include "message_types.h"

class MagicServer
{
   public:
      MagicServer(Network &network, UnstructuredBuffer &recv_buffer);
      ~MagicServer();

      void handlePacket(core_id_t core_id);

      UInt64 setFrequency(UInt64 core_number, UInt64 freq_in_mhz);
      UInt64 getFrequency(UInt64 core_number);
      bool inROI(void) const { return m_performance_enabled; }
      // To be called from MCP context
      UInt64 setInstrumentationMode(UInt64 sim_api_opt);

      // data type to hold arguments in a HOOK_MAGIC_MARKER callback
      struct MagicMarkerType {
         core_id_t core_id;
         UInt64 arg0, arg1;
      };
   private:
      Network &m_network;
      UnstructuredBuffer &m_recv_buffer;

      bool m_performance_enabled;
      LCPMagicOptTypes m_instrument_mode;

      UInt64 Magic(core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1);
      UInt64 setPerformance(bool enabled);
};

#endif // SYNC_SERVER_H
