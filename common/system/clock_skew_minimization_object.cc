#include "clock_skew_minimization_object.h"
#include "barrier_sync_client.h"
#include "barrier_sync_server.h"
#include "simulator.h"
#include "log.h"
#include "config.hpp"

ClockSkewMinimizationObject::Scheme
ClockSkewMinimizationObject::parseScheme(String scheme)
{
   if (scheme == "barrier")
      return BARRIER;
   else
   {
      config::Error("Unrecognized clock skew minimization scheme: %s", scheme.c_str());
   }
}

ClockSkewMinimizationClient*
ClockSkewMinimizationClient::create(Core* core)
{
   Scheme scheme = Sim()->getConfig()->getClockSkewMinimizationScheme();

   switch (scheme)
   {
      case BARRIER:
         return new BarrierSyncClient(core);

      default:
         LOG_PRINT_ERROR("Unrecognized scheme: %u", scheme);
         return (ClockSkewMinimizationClient*) NULL;
   }
}

ClockSkewMinimizationManager*
ClockSkewMinimizationManager::create()
{
   Scheme scheme = Sim()->getConfig()->getClockSkewMinimizationScheme();

   switch (scheme)
   {
      case BARRIER:
         return (ClockSkewMinimizationManager*) NULL;

      default:
         LOG_PRINT_ERROR("Unrecognized scheme: %u", scheme);
         return (ClockSkewMinimizationManager*) NULL;
   }
}

ClockSkewMinimizationServer*
ClockSkewMinimizationServer::create()
{
   Scheme scheme = Sim()->getConfig()->getClockSkewMinimizationScheme();

   switch (scheme)
   {
      case BARRIER:
         return new BarrierSyncServer();

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
