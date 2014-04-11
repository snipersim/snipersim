#include "memory_manager.h"
#include "stats.h"
#include "log.h"
#include "utils.h"
#include "fast_cache.h"

namespace FastNehalem
{

CacheBase *MemoryManager::l3cache = NULL, *MemoryManager::dram = NULL;

MemoryManager::MemoryManager(Core* core, Network* network, ShmemPerfModel* shmem_perf_model)
   : MemoryManagerFast(core, network, shmem_perf_model)
{
   if (!dram)
      dram = new Dram(core, "dram", 150);
   if (!l3cache)
      l3cache = new CacheLocked<16, 8192>(core, "L3", MemComponent::L3_CACHE, 35, dram);
   l2cache = new Cache<8, 256>(core, "L2", MemComponent::L2_CACHE, 9, l3cache);
   icache = new Cache<4, 32>(core, "L1-I", MemComponent::L1_ICACHE, 0, l2cache);
   dcache = new Cache<8, 32>(core, "L1-D", MemComponent::L1_DCACHE, 0, l2cache);
}

MemoryManager::~MemoryManager()
{
   delete icache;
   delete dcache;
   delete l2cache;
}

}
