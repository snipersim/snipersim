#include "nvm_cntlr.h"
#include "memory_manager.h"
#include "core.h"
#include "log.h"
#include "subsecond_time.h"
#include "stats.h"
#include "fault_injection.h"
#include "shmem_perf.h"
#include "config.h"           // Added by Kleber Kruger
#include "config.hpp"         // Added by Kleber Kruger
#include "nvm_perf_model.h"   // Added by Kleber Kruger

// #include "hit_where.h"

#if 0
   extern Lock iolock;
#  include "core_manager.h"
#  include "simulator.h"
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stdout); printf("[%s] %d%cdr %-25s@%3u: ", itostr(getShmemPerfModel()->getElapsedTime()).c_str(), getMemoryManager()->getCore()->getId(), Sim()->getCoreManager()->amiUserThread() ? '^' : '_', __FUNCTION__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); }
#else
#  define MYLOG(...) {}
#endif

class TimeDistribution;

namespace PrL1PrL2DramDirectoryMSI
{

NvmCntlr::NvmCntlr(MemoryManagerBase* memory_manager,
      ShmemPerfModel* shmem_perf_model,
      UInt32 cache_block_size)
   : DramCntlr(memory_manager, shmem_perf_model, cache_block_size)
   , m_logs(0)
   , m_log_ends(0)                               // m_log_rowbuffer_overflows
   , m_log_buffer(0)
   , m_log_size(NvmCntlr::getLogRowBufferSize()) // m_log_rowbuffer_size
   , m_log_disk_space(0)
   , m_log_max_disk_space(0)
   , m_log_type(NvmCntlr::getLogType()) // 
{
   // printf("LOGGING TYPE %d\n", m_log_type);
   registerStatsMetric("dram", memory_manager->getCore()->getId(), "logs", &m_logs);
   registerStatsMetric("dram", memory_manager->getCore()->getId(), "log_ends", &m_log_ends);
   registerStatsMetric("dram", memory_manager->getCore()->getId(), "log_max_disk_space", &m_log_max_disk_space);

   String path = Sim()->getConfig()->getOutputDirectory() + "/sim.logs.csv";
   if ((m_log_file = fopen(path.c_str(), "w")) == nullptr)
      fprintf(stderr, "Error on creating sim.ckpts.csv\n");
}

NvmCntlr::~NvmCntlr()
{
   fprintf(m_log_file, "%lu|%lu|%lu\n", m_logs, m_log_ends, m_log_disk_space);
   fclose(m_log_file);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NvmCntlr::getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now, ShmemPerf *perf)
{
   if (Sim()->getFaultinjectionManager())
   {
      if (m_data_map.count(address) == 0)
      {
         m_data_map[address] = new Byte[getCacheBlockSize()];
         memset((void*) m_data_map[address], 0x00, getCacheBlockSize());
      }

      // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into data_buf instead
      if (m_fault_injector)
         m_fault_injector->preRead(address, address, getCacheBlockSize(), (Byte*)m_data_map[address], now);

      memcpy((void*) data_buf, (void*) m_data_map[address], getCacheBlockSize());
   }

   SubsecondTime dram_access_latency = DramCntlr::runDramPerfModel(requester, now, address, READ, perf);

   if (loggingOnLoad()) 
   {
      HitWhere::where_t hit_where = HitWhere::MISS;
      SubsecondTime latency;
      boost::tie(latency, hit_where) = logDataToDram(address, requester, data_buf, now);

      // SubsecondTime total = dram_access_latency + latency;
      // printf("LOG AND LOAD | (%lu) latency: (%lu + %lu) = %lu\n", EpochManager::getGlobalSystemEID(), dram_access_latency.getNS(), latency.getNS(), total.getNS());
      
      dram_access_latency += latency;
   }
   else 
   {
      // printf("LOG | (%lu) latency = %lu\n", EpochManager::getGlobalSystemEID(), dram_access_latency.getNS());
   }

   ++m_reads;
   #ifdef ENABLE_DRAM_ACCESS_COUNT
   addToDramAccessCount(address, READ);
   #endif
   MYLOG("R @ %08lx latency %s", address, itostr(dram_access_latency).c_str());

   return boost::tuple<SubsecondTime, HitWhere::where_t>(dram_access_latency, HitWhere::DRAM);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NvmCntlr::putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   if (Sim()->getFaultinjectionManager())
   {
      if (m_data_map[address] == NULL)
      {
         LOG_PRINT_ERROR("Data Buffer does not exist");
      }
      memcpy((void*) m_data_map[address], (void*) data_buf, getCacheBlockSize());

      // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into data_buf instead
      if (m_fault_injector)
         m_fault_injector->postWrite(address, address, getCacheBlockSize(), (Byte*)m_data_map[address], now);
   }

   SubsecondTime dram_access_latency = DramCntlr::runDramPerfModel(requester, now, address, WRITE, &m_dummy_shmem_perf);

   if (loggingOnStore())
   {
      HitWhere::where_t hit_where = HitWhere::MISS;
      SubsecondTime latency;
      boost::tie(latency, hit_where) = logDataToDram(address, requester, data_buf, now);

      // SubsecondTime total = dram_access_latency + latency;
      // printf("LOG AND STORE | dram_latency + log_latency = total_latency: (%lu + %lu) = %lu\n", dram_access_latency.getNS(), latency.getNS(), total.getNS());
      
      dram_access_latency += latency;
   }
   else
   {
      // printf("STORE | (%lu) latency = %lu\n", EpochManager::getGlobalSystemEID(), dram_access_latency.getNS());
   }

   ++m_writes;
   #ifdef ENABLE_DRAM_ACCESS_COUNT
   addToDramAccessCount(address, WRITE);
   #endif
   MYLOG("W @ %08lx", address);

   return boost::tuple<SubsecondTime, HitWhere::where_t>(dram_access_latency, HitWhere::DRAM);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NvmCntlr::logDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   // TODO: Implement part of fault injection

   // TODO: Receive backup data (log data) and create corretly log entry
   createLogEntry(address, data_buf);

   UInt64 cache_block_size = getCacheBlockSize();
   SubsecondTime log_latency;

   // Filling the buffer...
   if (m_log_buffer + cache_block_size >= m_log_size)
   {
      log_latency = DramCntlr::runDramPerfModel(requester, now, address, LOG, &m_dummy_shmem_perf);
      m_log_ends++;
      m_log_buffer = 0;
   }
   else
   {
      m_log_buffer += cache_block_size;
      log_latency = SubsecondTime::Zero(); // TODO: terá algum custo o log em linha aberta?
   }

//   printf("LOG | Creating log to entry %lu (log_latency = %lu)\n", address, log_latency.getNS());

   ++m_logs;
   #ifdef ENABLE_DRAM_ACCESS_COUNT
   addToDramAccessCount(address, LOG);
   #endif
   MYLOG("L @ %08lx", address);

   return boost::tuple<SubsecondTime, HitWhere::where_t>(log_latency, HitWhere::DRAM);
}

void
NvmCntlr::printDramAccessCount()
{
   for (UInt32 k = 0; k < DramCntlrInterface::NUM_ACCESS_TYPES; k++)
   {
      for (AccessCountMap::iterator i = m_dram_access_count[k].begin(); i != m_dram_access_count[k].end(); i++)
      {
         if ((*i).second > 100)
         {
            LOG_PRINT("Dram Cntlr(%i), Address(0x%x), Access Count(%llu), Access Type(%s)",
                  m_memory_manager->getCore()->getId(), (*i).first, (*i).second,
                  (k == READ)? "READ" : (k == WRITE)? "WRITE" : "LOG");
         }
      }
   }
}

// TODO: implement this method called on checkpoint events
void
NvmCntlr::checkpoint()
{
   // printf("CHECKPOINT %lu | DATA LENGTH: %lu KB | logs: %lu\n", EpochManager::getGlobalSystemEID(), m_log_disk_space, m_logs);
//    SubsecondTime dram_access_latency = runDramPerfModel(requester, now, address, LOG, &m_dummy_shmem_perf);
   m_log_ends++;
   m_log_buffer = 0;

   fprintf(m_log_file, "%lu\n", m_log_disk_space);
   
   if (m_log_disk_space > m_log_max_disk_space)
   {
   //    printf("max before: %lu\n", m_log_max_disk_space);
      m_log_max_disk_space = m_log_disk_space;
   //    printf("max after: %lu\n", m_log_max_disk_space);
   }    
   m_log_disk_space = 0;
}

void
NvmCntlr::createLogEntry(IntPtr address, Byte* data_buf)
{
   // UInt64 eid = EpochManager::getGlobalSystemEID();
   // printf("Creating log entry for epoch: %lu { metadata: %lu, data: %u }\n", eid, address, (unsigned int) *data_buf);
   m_log_disk_space += getCacheBlockSize();
}

bool
NvmCntlr::isLogEnabled()
{
   return m_log_type != LOGGING_DISABLED;
}

bool
NvmCntlr::loggingOnLoad()
{
   return m_log_type == LOGGING_ON_LOAD;
}

bool
NvmCntlr::loggingOnStore()
{
   return m_log_type == LOGGING_ON_STORE;
}

NvmCntlr::log_type_t
NvmCntlr::getLogType()
{
   String param = "perf_model/dram/log_type";
   if (!Sim()->getCfg()->hasKey(param) || Sim()->getCfg()->getString(param) == "disabled")
      return NvmCntlr::LOGGING_DISABLED;

   String value = Sim()->getCfg()->getString(param);
   if (value == "read")
      return NvmCntlr::LOGGING_ON_LOAD;
   else if (value == "write")
      return NvmCntlr::LOGGING_ON_STORE;
   else if (value == "cmd")
      return NvmCntlr::LOGGING_ON_COMMAND;
   
   assert(false);
}

UInt32
NvmCntlr::getLogRowBufferSize()
{
   String param = "perf_model/dram/log_row_buffer_size";
   return Sim()->getCfg()->hasKey(param) ? Sim()->getCfg()->getInt(param) : 1024;
}

const char *NvmCntlr::LogTypeString(NvmCntlr::LogType type)
{
   switch (type)
   {
      case LOGGING_DISABLED:     return "LOGGING_DISABLED";
      case LOGGING_ON_LOAD:      return "LOGGING_ON_LOAD";
      case LOGGING_ON_STORE:     return "LOGGING_ON_STORE";
      case LOGGING_ON_COMMAND:   return "LOGGING_ON_COMMAND";
      default:                   return "?";
   }
}

}
