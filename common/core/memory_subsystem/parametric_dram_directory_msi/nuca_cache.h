#ifndef __NUCA_CACHE_H
#define __NUCA_CACHE_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "hit_where.h"
#include "cache_cntlr.h"

#include "boost/tuple/tuple.hpp"

class MemoryManagerBase;
class ShmemPerfModel;
class AddressHomeLookup;
class QueueModel;
class ShmemPerf;

class NucaCache
{
   private:
      core_id_t m_core_id;
      MemoryManagerBase *m_memory_manager;
      ShmemPerfModel *m_shmem_perf_model;
      AddressHomeLookup *m_home_lookup;
      UInt32 m_cache_block_size;
      ComponentLatency m_data_access_time;
      ComponentLatency m_tags_access_time;
      ComponentBandwidth m_data_array_bandwidth;

      Cache* m_cache;
      QueueModel *m_queue_model;

      UInt64 m_reads, m_writes, m_read_misses, m_write_misses;

      SubsecondTime accessDataArray(Cache::access_t access, SubsecondTime t_start, ShmemPerf *perf);

   public:
      NucaCache(MemoryManagerBase* memory_manager, ShmemPerfModel* shmem_perf_model, AddressHomeLookup* home_lookup, UInt32 cache_block_size, ParametricDramDirectoryMSI::CacheParameters& parameters);
      ~NucaCache();

      boost::tuple<SubsecondTime, HitWhere::where_t> read(IntPtr address, Byte* data_buf, SubsecondTime now, ShmemPerf *perf);
      boost::tuple<SubsecondTime, HitWhere::where_t> write(IntPtr address, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now);
};

#endif // __NUCA_CACHE_H
