#ifndef NETWORK_MODEL_ANALYTICAL_H
#define NETWORK_MODEL_ANALYTICAL_H

#include "network.h"
#include "lock.h"
#include "network_model_analytical_params.h"
#include "subsecond_time.h"

class NetworkModelAnalytical : public NetworkModel
{
   public:
      NetworkModelAnalytical(Network *net, EStaticNetwork net_type);
      ~NetworkModelAnalytical();

      void routePacket(const NetPacket &pkt,
                       std::vector<Hop> &nextHops);
      void processReceivedPacket(NetPacket& pkt) {}

      void outputSummary(std::ostream &out);

      void enable();
      void disable();

   private:
      SubsecondTime computeLatency(const NetPacket &);
      void updateUtilization();
      static void receiveMCPUpdate(void *, NetPacket);

      UInt64 _bytesSent;
      UInt32 _bytesRecv;

      SubsecondTime _cyclesProc;
      SubsecondTime _cyclesLatency;
      SubsecondTime _cyclesContention;
      UInt64 _procCost;

      double _globalUtilization;
      SubsecondTime _localUtilizationLastUpdate;
      UInt64 _localUtilizationFlitsSent;
      UInt64 _updateInterval;

      Lock _lock;

      NetworkModelAnalyticalParameters m_params;

      bool m_enabled;
};

#endif // NETWORK_MODEL_ANALYTICAL_H
