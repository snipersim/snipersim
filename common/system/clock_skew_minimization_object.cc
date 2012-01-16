#include "core.h"
#include "network.h"
#include "transport.h"
#include "packetize.h"

#include "clock_skew_minimization_object.h"
#include "barrier_sync_client.h"
#include "barrier_sync_server.h"
#include "ring_sync_client.h"
#include "ring_sync_manager.h"
#include "random_pairs_sync_client.h"
#include "finegrained_sync_client.h"

#include "log.h"

void ClockSkewMinimizationClientNetworkCallback(void* obj, NetPacket packet)
{
   ClockSkewMinimizationClient *client = (ClockSkewMinimizationClient*) obj;
   assert(client != NULL);

   client->netProcessSyncMsg(packet);
}

ClockSkewMinimizationObject::Scheme
ClockSkewMinimizationObject::parseScheme(String scheme)
{
   if (scheme == "barrier")
      return BARRIER;
   else if (scheme == "ring")
      return RING;
   else if (scheme == "random_pairs")
      return RANDOM_PAIRS;
   else if (scheme == "none")
      return NONE;
   else if (scheme == "finegrained")
      return FINE_GRAINED;
   else
   {
      LOG_PRINT_ERROR("Unrecognized clock skew minimization scheme: %s", scheme.c_str());
      return NUM_SCHEMES;
   }
}

ClockSkewMinimizationClient*
ClockSkewMinimizationClient::create(String scheme_str, Core* core)
{
   Scheme scheme = ClockSkewMinimizationObject::parseScheme(scheme_str);

   switch (scheme)
   {
      case BARRIER:
         return new BarrierSyncClient(core);

      case RING:
         return new RingSyncClient(core);

      case RANDOM_PAIRS:
         return new RandomPairsSyncClient(core);

      case NONE:
         return (ClockSkewMinimizationClient*) NULL;

      case FINE_GRAINED:
         return new FinegrainedSyncClient(core);

      default:
         LOG_PRINT_ERROR("Unrecognized scheme: %u", scheme);
         return (ClockSkewMinimizationClient*) NULL;
   }
}

ClockSkewMinimizationManager*
ClockSkewMinimizationManager::create(String scheme_str)
{
   Scheme scheme = ClockSkewMinimizationObject::parseScheme(scheme_str);

   switch (scheme)
   {
      case BARRIER:
         return (ClockSkewMinimizationManager*) NULL;

      case RING:
         return new RingSyncManager();

      case RANDOM_PAIRS:
         return (ClockSkewMinimizationManager*) NULL;

      case NONE:
         return (ClockSkewMinimizationManager*) NULL;

      case FINE_GRAINED:
         return (ClockSkewMinimizationManager*) NULL;

      default:
         LOG_PRINT_ERROR("Unrecognized scheme: %u", scheme);
         return (ClockSkewMinimizationManager*) NULL;
   }
}

ClockSkewMinimizationServer*
ClockSkewMinimizationServer::create(String scheme_str, Network& network, UnstructuredBuffer& recv_buff)
{
   Scheme scheme = ClockSkewMinimizationObject::parseScheme(scheme_str);

   switch (scheme)
   {
      case BARRIER:
         return new BarrierSyncServer(network, recv_buff);

      case RING:
         return (ClockSkewMinimizationServer*) NULL;

      case RANDOM_PAIRS:
         return (ClockSkewMinimizationServer*) NULL;

      case NONE:
         return (ClockSkewMinimizationServer*) NULL;

      case FINE_GRAINED:
         return (ClockSkewMinimizationServer*) NULL;

      default:
         LOG_PRINT_ERROR("Unrecognized scheme: %u", scheme);
         return (ClockSkewMinimizationServer*) NULL;
   }
}

SubsecondTime
ClockSkewMinimizationServer::getGlobalTime()
{
   LOG_PRINT_ERROR("This clock skew minimization server does not support getGlobalTime");
}
