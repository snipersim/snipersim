#include "prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "simple_prefetcher.h"
#include "ghb_prefetcher.h"
#include "a53prefetcher.h"
#include "ip_stride_prefetcher.h"
#include "stream_prefetcher.h"
#include "spp_prefetcher.h"
#include "sms_prefetcher.h"
#include "berti_prefetcher.h"
#include "ipcp_prefetcher.h"

Prefetcher* Prefetcher::createPrefetcher(String type, String configName, core_id_t core_id, UInt32 shared_cores)
{
   if (type == "none")
      return NULL;
   else if (type == "simple")
      return new SimplePrefetcher(configName, core_id, shared_cores);
   else if (type == "ghb")
      return new GhbPrefetcher(configName, core_id);
   else if (type == "a53prefetcher")
       return new A53Prefetcher(configName, core_id);
   else if (type == "ip_stride")
      return new IPStridePrefetcher(configName, core_id);
   else if (type == "streamer")
      return new Streamer(configName, core_id);
   else if (type == "spp")
      return new SPPPrefetcher(configName, core_id);
   else if (type == "sms")
      return new SMSPrefetcher(configName, core_id);
   else if (type == "berti")
      return new BertiPrefetcher(configName, core_id);
   else if (type == "ipcp")
      return new IPCPPrefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
}
