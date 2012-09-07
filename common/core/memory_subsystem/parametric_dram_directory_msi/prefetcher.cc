#include "prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "simple_prefetcher.h"
#include "ghb_prefetcher.h"

Prefetcher* Prefetcher::createPrefetcher(String type, String configName, core_id_t core_id)
{
   if (type == "none")
      return NULL;
   else if (type == "simple")
      return new SimplePrefetcher(configName, core_id);
   else if (type == "ghb")
      return new GhbPrefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
}
