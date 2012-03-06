#include "dram_cntlr.h"
#include "memory_manager.h"
#include "core.h"
#include "log.h"
#include "subsecond_time.h"
#include "stats.h"

#if 0
   extern Lock iolock;
#  include "core_manager.h"
#  include "simulator.h"
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stdout); printf("[%s] %d%cdr %-25s@%3u: ", itostr(getShmemPerfModel()->getElapsedTime()).c_str(), getMemoryManager()->getCore()->getId(), Sim()->getCoreManager()->amiUserThread() ? '^' : '_', __FUNCTION__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); }
#else
#  define MYLOG(...) {}
#endif

namespace PrL1PrL2DramDirectoryMSI
{

DramCntlr::DramCntlr(MemoryManagerBase* memory_manager,
      SubsecondTime dram_access_cost,
      ComponentBandwidth dram_bandwidth,
      bool dram_queue_model_enabled,
      String dram_queue_model_type,
      UInt32 cache_block_size,
      ShmemPerfModel* shmem_perf_model):
   m_memory_manager(memory_manager),
   m_cache_block_size(cache_block_size),
   m_shmem_perf_model(shmem_perf_model),
   m_reads(0),
   m_writes(0)
{
   m_dram_perf_model = new DramPerfModel(
         memory_manager->getCore()->getId(),
         dram_access_cost,
         dram_bandwidth,
         dram_queue_model_enabled,
         dram_queue_model_type,
         cache_block_size);

   m_dram_access_count = new AccessCountMap[NUM_ACCESS_TYPES];
   registerStatsMetric("dram", memory_manager->getCore()->getId(), "reads", &m_reads);
   registerStatsMetric("dram", memory_manager->getCore()->getId(), "writes", &m_writes);
}

DramCntlr::~DramCntlr()
{
   printDramAccessCount();
   delete [] m_dram_access_count;

   delete m_dram_perf_model;
}

void
DramCntlr::getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   if (m_data_map[address] == NULL)
   {
      m_data_map[address] = new Byte[getCacheBlockSize()];
      memset((void*) m_data_map[address], 0x00, getCacheBlockSize());
   }

   memcpy((void*) data_buf, (void*) m_data_map[address], getCacheBlockSize());

   SubsecondTime dram_access_latency = runDramPerfModel(requester);
   getShmemPerfModel()->incrElapsedTime(dram_access_latency, ShmemPerfModel::_SIM_THREAD);

   ++m_reads;
   #ifdef ENABLE_DRAM_ACCESS_COUNT
   addToDramAccessCount(address, READ);
   #endif
   MYLOG("R @ %08lx latency %s", address, itostr(dram_access_latency).c_str());
}

void
DramCntlr::putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   if (m_data_map[address] == NULL)
   {
      LOG_PRINT_ERROR("Data Buffer does not exist");
   }
   memcpy((void*) m_data_map[address], (void*) data_buf, getCacheBlockSize());

   runDramPerfModel(requester);

   ++m_writes;
   #ifdef ENABLE_DRAM_ACCESS_COUNT
   addToDramAccessCount(address, WRITE);
   #endif
   MYLOG("W @ %08lx", address);
}

SubsecondTime
DramCntlr::runDramPerfModel(core_id_t requester)
{
   SubsecondTime pkt_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);
   UInt64 pkt_size = getCacheBlockSize();
   SubsecondTime dram_access_latency = m_dram_perf_model->getAccessLatency(pkt_time, pkt_size, requester);
   return dram_access_latency;
}

void
DramCntlr::addToDramAccessCount(IntPtr address, access_t access_type)
{
   m_dram_access_count[access_type][address] = m_dram_access_count[access_type][address] + 1;
}

void
DramCntlr::printDramAccessCount()
{
   for (UInt32 k = 0; k < NUM_ACCESS_TYPES; k++)
   {
      for (AccessCountMap::iterator i = m_dram_access_count[k].begin(); i != m_dram_access_count[k].end(); i++)
      {
         if ((*i).second > 100)
         {
            LOG_PRINT("Dram Cntlr(%i), Address(0x%x), Access Count(%llu), Access Type(%s)",
                  m_memory_manager->getCore()->getId(), (*i).first, (*i).second,
                  (k == READ)? "READ" : "WRITE");
         }
      }
   }
}

}
