#include "cache_cntlr.h"
#include "log.h"
#include "memory_manager.h"
#include "core_manager.h"
#include "simulator.h"
#include "subsecond_time.h"
#include "config.hpp"
#include "fault_injection.h"

// Define to allow private L2 caches not to take the stack lock.
// Works in most cases, but seems to have some more bugs or race conditions, preventing it from being ready for prime time.
//#define PRIVATE_L2_OPTIMIZATION

Lock iolock;
#if 0
#  define LOCKED(...) { ScopedLock sl(iolock); fflush(stderr); __VA_ARGS__; fflush(stderr); }
#  define LOGID() fprintf(stderr, "[%s] %2u%c [ %2d(%2d)-L%u%c ] %-25s@%3u: ", \
                     itostr(getShmemPerfModel()->getElapsedTime()).c_str(), Sim()->getCoreManager()->getCurrentCoreID(), \
                     Sim()->getCoreManager()->amiUserThread() ? '^' : '_', \
                     m_core_id_master, m_core_id, m_mem_component < MemComponent::L2_CACHE ? 1 : m_mem_component - MemComponent::L2_CACHE + 2, \
                     m_mem_component == MemComponent::L1_ICACHE ? 'I' : (m_mem_component == MemComponent::L1_DCACHE  ? 'D' : ' '),  \
                     __FUNCTION__, __LINE__ \
                  );
#  define MYLOG(...) LOCKED(LOGID(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");)
#  define DUMPDATA(data_buf, data_length) { for(UInt32 i = 0; i < data_length; ++i) fprintf(stderr, "%02x ", data_buf[i]); }
#else
#  define MYLOG(...) {}
#endif

namespace ParametricDramDirectoryMSI
{

char CStateString(CacheState::cstate_t cstate) {
   switch(cstate)
   {
      case CacheState::INVALID:           return 'I';
      case CacheState::SHARED:            return 'S';
      case CacheState::MODIFIED:          return 'M';
      case CacheState::OWNED:             return 'O';
      case CacheState::INVALID_COLD:      return '_';
      case CacheState::INVALID_EVICT:     return 'e';
      case CacheState::INVALID_COHERENCY: return 'c';
      default:                            return '?';
   }
}

const char * ReasonString(Transition::reason_t reason) {
   switch(reason)
   {
      case Transition::CORE_RD:     return "read";
      case Transition::CORE_RDEX:   return "readex";
      case Transition::CORE_WR:     return "write";
      case Transition::UPGRADE:     return "upgrade";
      case Transition::EVICT:       return "evict";
      case Transition::EVICT_LOWER: return "evictlower";
      case Transition::COHERENCY:   return "coherency";
      default:                      return "other";
   }
}

MshrEntry make_mshr(SubsecondTime t_issue, SubsecondTime t_complete) {
   MshrEntry e;
   e.t_issue = t_issue; e.t_complete = t_complete;
   return e;
}

#ifdef ENABLE_TRACK_SHARING_PREVCACHES
PrevCacheIndex CacheCntlrList::find(core_id_t core_id, MemComponent::component_t mem_component)
{
   for(PrevCacheIndex idx = 0; idx < size(); ++idx)
      if ((*this)[idx]->m_core_id == core_id && (*this)[idx]->m_mem_component == mem_component)
         return idx;
   LOG_PRINT_ERROR("");
}
#endif

void CacheMasterCntlr::createSetLocks(UInt32 cache_block_size, UInt32 num_sets, UInt32 core_offset, UInt32 num_cores)
{
   m_log_blocksize = floorLog2(cache_block_size);
   m_num_sets = num_sets;
   m_setlocks.resize(m_num_sets, SetLock(core_offset, num_cores));
}

SetLock*
CacheMasterCntlr::getSetLock(IntPtr addr)
{
   return &m_setlocks.at((addr >> m_log_blocksize) & (m_num_sets-1));
}

CacheMasterCntlr::~CacheMasterCntlr()
{
}

CacheCntlr::CacheCntlr(MemComponent::component_t mem_component,
      String name,
      core_id_t core_id,
      MemoryManager* memory_manager,
      AddressHomeLookup* dram_directory_home_lookup,
      Semaphore* user_thread_sem,
      Semaphore* network_thread_sem,
      UInt32 cache_block_size,
      CacheParameters & cache_params,
      ShmemPerfModel* shmem_perf_model):
   m_enabled(false),
   m_mem_component(mem_component),
   m_memory_manager(memory_manager),
   m_next_cache_cntlr(NULL),
   m_last_level(NULL),
   m_dram_directory_home_lookup(dram_directory_home_lookup),
   m_perfect(cache_params.perfect),
   m_coherent(cache_params.coherent),
   m_prefetch_on_prefetch_hit(false),
   m_l1_mshr(cache_params.outstanding_misses > 0),
   m_core_id(core_id),
   m_cache_block_size(cache_block_size),
   m_cache_writethrough(cache_params.writethrough),
   m_writeback_time(cache_params.writeback_time),
   m_next_level_read_bandwidth(cache_params.next_level_read_bandwidth),
   m_shared_cores(cache_params.shared_cores),
   m_user_thread_sem(user_thread_sem),
   m_network_thread_sem(network_thread_sem),
   m_last_remote_hit_where(HitWhere::UNKNOWN),
   m_shmem_perf_model(shmem_perf_model)
{
   m_core_id_master = m_core_id - m_core_id % m_shared_cores;
   Sim()->getStatsManager()->logTopology(name, core_id, m_core_id_master);

   LOG_ASSERT_ERROR(!Sim()->getCfg()->hasKey("perf_model/perfect_llc"),
                    "perf_model/perfect_llc is deprecated, use perf_model/lX_cache/perfect instead");

   if (isMasterCache())
   {
      /* Master cache */
      m_master = new CacheMasterCntlr(name, core_id, cache_params.outstanding_misses);
      m_master->m_cache = new Cache(name,
            cache_params.size,
            cache_params.associativity,
            m_cache_block_size,
            cache_params.replacement_policy,
            CacheBase::SHARED_CACHE,
            CacheBase::parseAddressHash(cache_params.hash_function),
            Sim()->getFaultinjectionManager()
               ? Sim()->getFaultinjectionManager()->getFaultInjector(m_core_id_master, mem_component)
               : NULL);
      m_master->m_prefetcher = Prefetcher::createPrefetcher(cache_params.prefetcher, cache_params.configName, m_core_id);
   }
   else
   {
      /* Shared, non-master cache, we're just a proxy */
      m_master = getMemoryManager()->getCacheCntlrAt(m_core_id_master, mem_component)->m_master;
   }

   if (m_master->m_prefetcher)
      m_prefetch_on_prefetch_hit = Sim()->getCfg()->getBoolArray("perf_model/l2_cache/prefetcher/prefetch_on_prefetch_hit", core_id);

   bzero(&stats, sizeof(stats));

   registerStatsMetric(name, core_id, "loads", &stats.loads);
   registerStatsMetric(name, core_id, "stores", &stats.stores);
   registerStatsMetric(name, core_id, "load-misses", &stats.load_misses);
   registerStatsMetric(name, core_id, "store-misses", &stats.store_misses);
   // Does not work for loads, since the interval core model doesn't issue the loads until after the first miss has completed
   registerStatsMetric(name, core_id, "load-overlapping-misses", &stats.load_overlapping_misses);
   registerStatsMetric(name, core_id, "store-overlapping-misses", &stats.store_overlapping_misses);
   registerStatsMetric(name, core_id, "loads-prefetch", &stats.loads_prefetch);
   registerStatsMetric(name, core_id, "stores-prefetch", &stats.stores_prefetch);
   registerStatsMetric(name, core_id, "hits-prefetch", &stats.hits_prefetch);
   registerStatsMetric(name, core_id, "evict-prefetch", &stats.evict_prefetch);
   registerStatsMetric(name, core_id, "evict-shared", &stats.evict_shared);
   registerStatsMetric(name, core_id, "evict-modified", &stats.evict_modified);
   registerStatsMetric(name, core_id, "total-latency", &stats.total_latency);
   registerStatsMetric(name, core_id, "snoop-latency", &stats.snoop_latency);
   registerStatsMetric(name, core_id, "mshr-latency", &stats.mshr_latency);
   registerStatsMetric(name, core_id, "prefetches", &stats.prefetches);
   for(CacheState::cstate_t state = CacheState::CSTATE_FIRST; state < CacheState::NUM_CSTATE_STATES; state = CacheState::cstate_t(int(state)+1)) {
      registerStatsMetric(name, core_id, String("loads-")+CStateString(state), &stats.loads_state[state]);
      registerStatsMetric(name, core_id, String("stores-")+CStateString(state), &stats.stores_state[state]);
      registerStatsMetric(name, core_id, String("load-misses-")+CStateString(state), &stats.load_misses_state[state]);
      registerStatsMetric(name, core_id, String("store-misses-")+CStateString(state), &stats.store_misses_state[state]);
   }
   if (mem_component == MemComponent::L1_ICACHE || mem_component == MemComponent::L1_DCACHE) {
      for(HitWhere::where_t hit_where = HitWhere::WHERE_FIRST; hit_where < HitWhere::NUM_HITWHERES; hit_where = HitWhere::where_t(int(hit_where)+1)) {
         const char * where_str = HitWhereString(hit_where);
         if (where_str[0] == '?') continue;
         registerStatsMetric(name, core_id, String("loads-where-")+where_str, &stats.loads_where[hit_where]);
         registerStatsMetric(name, core_id, String("stores-where-")+where_str, &stats.stores_where[hit_where]);
      }
   }
#ifdef ENABLE_TRANSITIONS
   for(CacheState::cstate_t old_state = CacheState::CSTATE_FIRST; old_state < CacheState::NUM_CSTATE_STATES; old_state = CacheState::cstate_t(int(old_state)+1))
      for(CacheState::cstate_t new_state = CacheState::CSTATE_FIRST; new_state < CacheState::NUM_CSTATE_STATES; new_state = CacheState::cstate_t(int(new_state)+1))
         registerStatsMetric(name, core_id, String("transitions-")+CStateString(old_state)+"-"+CStateString(new_state), &stats.transitions[old_state][new_state]);
   for(Transition::reason_t reason = Transition::REASON_FIRST; reason < Transition::NUM_REASONS; reason = Transition::reason_t(int(reason)+1))
      for(CacheState::cstate_t old_state = CacheState::CSTATE_FIRST; old_state < CacheState::NUM_CSTATE_SPECIAL_STATES; old_state = CacheState::cstate_t(int(old_state)+1))
         for(CacheState::cstate_t new_state = CacheState::CSTATE_FIRST; new_state < CacheState::NUM_CSTATE_SPECIAL_STATES; new_state = CacheState::cstate_t(int(new_state)+1))
            registerStatsMetric(name, core_id, String("transitions-")+ReasonString(reason)+"-"+CStateString(old_state)+"-"+CStateString(new_state), &stats.transition_reasons[reason][old_state][new_state]);
#endif
}

CacheCntlr::~CacheCntlr()
{
   if (isMasterCache()) {
      delete m_master->m_cache;
      delete m_master;
   }
   #ifdef TRACK_LATENCY_BY_HITWHERE
   for(std::unordered_map<HitWhere::where_t, StatHist>::iterator it = lat_by_where.begin(); it != lat_by_where.end(); ++it) {
      printf("%2u-%s: ", m_core_id, HitWhereString(it->first));
      it->second.print();
   }
   #endif
}

void
CacheCntlr::setPrevCacheCntlrs(CacheCntlrList& prev_cache_cntlrs)
{
   /* Append our prev_caches list to the master one (only master nodes) */
   for(CacheCntlrList::iterator it = prev_cache_cntlrs.begin(); it != prev_cache_cntlrs.end(); it++)
      if ((*it)->isMasterCache())
         m_master->m_prev_cache_cntlrs.push_back(*it);
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   LOG_ASSERT_ERROR(m_master->m_prev_cache_cntlrs.size() <= MAX_NUM_PREVCACHES, "shared locations vector too small, increase MAX_NUM_PREVCACHES to at least %u", m_master->m_prev_cache_cntlrs.size());
   #endif
}



/*****************************************************************************
 * operations called by core on first-level cache
 *****************************************************************************/

HitWhere::where_t
CacheCntlr::processMemOpFromCore(
      Core::lock_signal_t lock_signal,
      Core::mem_op_t mem_op_type,
      IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length,
      bool modeled)
{
   HitWhere::where_t hit_where = HitWhere::MISS;

   // Protect against concurrent access from sibling SMT threads
   ScopedLock sl_smt(m_master->m_smt_lock);

   LOG_PRINT("processMemOpFromCore(), lock_signal(%u), mem_op_type(%u), ca_address(0x%x)",
             lock_signal, mem_op_type, ca_address);
MYLOG("----------------------------------------------");
MYLOG("%c%c %lx+%u..+%u", mem_op_type == Core::WRITE ? 'W' : 'R', mem_op_type == Core::READ_EX ? 'X' : ' ', ca_address, offset, data_length);
LOG_ASSERT_ERROR((ca_address & (getCacheBlockSize() - 1)) == 0, "address at cache line + %x", ca_address & (getCacheBlockSize() - 1));
LOG_ASSERT_ERROR(offset + data_length <= getCacheBlockSize(), "access until %u > %u", offset + data_length, getCacheBlockSize());

   #ifdef PRIVATE_L2_OPTIMIZATION
   /* if this is the second part of an atomic operation: we already have the lock, don't lock again */
   if (lock_signal != Core::UNLOCK)
      acquireLock(ca_address);
   #else
   /* if we'll need the next level (because we're a writethrough cache, and either this is a write
      or we're part of an atomic pair in which this or the other memop is potentially a write):
      make sure to lock it now, so the cache line in L2 doesn't fall from under us
      between operationPermissibleinCache and the writethrough */
   bool lock_all = m_cache_writethrough && ((mem_op_type == Core::WRITE) || (lock_signal != Core::NONE));

    /* if this is the second part of an atomic operation: we already have the lock, don't lock again */
   if (lock_signal != Core::UNLOCK) {
      if (lock_all)
         acquireStackLock(ca_address);
      else
         acquireLock(ca_address);
   }
   #endif

   SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);

   bool cache_hit = operationPermissibleinCache(ca_address, mem_op_type);

   if (!cache_hit && m_perfect)
   {
      cache_hit = true;
      hit_where = HitWhere::where_t(m_mem_component);
      SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(ca_address);
      if (cache_block_info)
         cache_block_info->setCState(CacheState::MODIFIED);
      else
         insertCacheBlock(ca_address, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, NULL, ShmemPerfModel::_USER_THREAD);
   }

   if (modeled && m_enabled)
   {
      ScopedLock sl(getLock());
      // Update the Cache Counters
      getCache()->updateCounters(cache_hit);
      updateCounters(mem_op_type, ca_address, cache_hit, getCacheState(ca_address), Prefetch::NONE);
   }

   if (cache_hit)
   {
MYLOG("L1 hit");
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_USER_THREAD);
      hit_where = (HitWhere::where_t)m_mem_component;

      if (modeled && m_l1_mshr)
      {
         ScopedLock sl(getLock());
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         SubsecondTime t_completed = m_master->m_l1_mshr.getTagCompletionTime(ca_address);
         if (t_completed != SubsecondTime::MaxTime() && t_completed > t_now)
         {
            if (mem_op_type == Core::WRITE)
               ++stats.store_overlapping_misses;
            else
               ++stats.load_overlapping_misses;

            SubsecondTime latency = t_completed - t_now;
            getShmemPerfModel()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
      }

   } else {
      /* cache miss */
MYLOG("L1 miss");
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

      SubsecondTime t_miss_begin = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
      SubsecondTime t_mshr_avail = t_miss_begin;
      if (modeled && m_l1_mshr)
      {
         ScopedLock sl(getLock());
         t_mshr_avail = m_master->m_l1_mshr.getStartTime(t_miss_begin);
         LOG_ASSERT_ERROR(t_mshr_avail >= t_miss_begin, "t_mshr_avail < t_miss_begin");
         SubsecondTime mshr_latency = t_mshr_avail - t_miss_begin;
         // Delay until we have an empty slot in the MSHR
         getShmemPerfModel()->incrElapsedTime(mshr_latency, ShmemPerfModel::_USER_THREAD);
         stats.mshr_latency += mshr_latency;
      }

      if (lock_signal == Core::UNLOCK)
         LOG_PRINT_ERROR("Expected to find address(0x%x) in L1 Cache", ca_address);

      #ifdef PRIVATE_L2_OPTIMIZATION
      #else
      if (!lock_all)
         acquireStackLock(ca_address, true);
      #endif

      // Invalidate the cache block before passing the request to L2 Cache
      if (getCacheState(ca_address) != CacheState::INVALID)
         invalidateCacheBlock(ca_address);

MYLOG("processMemOpFromCore l%d before next", m_mem_component);
      hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, modeled, Prefetch::NONE, t_start, false);
      bool next_cache_hit = hit_where != HitWhere::MISS;
MYLOG("processMemOpFromCore l%d next hit = %d", m_mem_component, next_cache_hit);

      if (next_cache_hit) {

      } else {
         /* last level miss, a message has been sent. */

MYLOG("processMemOpFromCore l%d waiting for sent message", m_mem_component);
         #ifdef PRIVATE_L2_OPTIMIZATION
         releaseLock(ca_address);
         #else
         releaseStackLock(ca_address);
         #endif

         waitForNetworkThread();
MYLOG("processMemOpFromCore l%d postwakeup", m_mem_component);

         //acquireStackLock(ca_address);
         // Pass stack lock through from network thread

         wakeUpNetworkThread();
MYLOG("processMemOpFromCore l%d got message reply", m_mem_component);

         /* have the next cache levels fill themselves with the new data */
MYLOG("processMemOpFromCore l%d before next fill", m_mem_component);
         hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, false, Prefetch::NONE, t_start, true);
MYLOG("processMemOpFromCore l%d after next fill", m_mem_component);
         LOG_ASSERT_ERROR(hit_where != HitWhere::MISS,
            "Tried to read in next-level cache, but data is already gone");

         #ifdef PRIVATE_L2_OPTIMIZATION
         releaseStackLock(ca_address, true);
         #else
         #endif
      }


      /* data should now be in next-level cache, go get it */
      SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
      copyDataFromNextLevel(mem_op_type, ca_address, modeled, t_now);

      #ifdef PRIVATE_L2_OPTIMIZATION
      #else
      if (!lock_all)
         releaseStackLock(ca_address, true);
      #endif

      LOG_ASSERT_ERROR(operationPermissibleinCache(ca_address, mem_op_type),
         "Expected %x to be valid in L1", ca_address);


      if (modeled && m_l1_mshr)
      {
         SubsecondTime t_miss_end = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         ScopedLock sl(getLock());
         m_master->m_l1_mshr.getCompletionTime(t_miss_begin, t_miss_end - t_mshr_avail, ca_address);
      }
   }


   accessCache(mem_op_type, ca_address, offset, data_buf, data_length);
MYLOG("access done");


   SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   SubsecondTime total_latency = t_now - t_start;

   // From here on downwards: not long anymore, only stats update so blanket cntrl lock
   {
      ScopedLock sl(getLock());

      if (! cache_hit && modeled) {
         stats.total_latency += total_latency;
      }

      #ifdef TRACK_LATENCY_BY_HITWHERE
      if (modeled && m_enabled)
         lat_by_where[hit_where].update(total_latency);
      #endif

      /* if this is the first part of an atomic operation: keep the lock(s) */
      #ifdef PRIVATE_L2_OPTIMIZATION
      if (lock_signal != Core::LOCK)
         releaseLock(ca_address);
      #else
      if (lock_signal != Core::LOCK) {
         if (lock_all)
            releaseStackLock(ca_address);
         else
            releaseLock(ca_address);
      }
      #endif

      if (mem_op_type == Core::WRITE)
         stats.stores_where[hit_where]++;
      else
         stats.loads_where[hit_where]++;
   }


   // Call Prefetch on next-level caches (but not for atomic instructions as that causes a locking mess)
   if (lock_signal != Core::LOCK && modeled && m_next_cache_cntlr)
   {
      m_next_cache_cntlr->Prefetch(t_start);
   }


   MYLOG("returning %s, latency %lu ns", HitWhereString(hit_where), total_latency.getNS());
   return hit_where;
}


void
CacheCntlr::updateHits(Core::mem_op_t mem_op_type, UInt64 hits)
{
   ScopedLock sl(getLock());

   while(hits > 0)
   {
      getCache()->updateCounters(true);
      updateCounters(mem_op_type, 0, true, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, Prefetch::NONE);
      hits--;
   }
}


void
CacheCntlr::copyDataFromNextLevel(Core::mem_op_t mem_op_type, IntPtr address, bool modeled, SubsecondTime t_now)
{
   // TODO: what if it's already gone? someone else may invalitate it between the time it arrived an when we get here...
   LOG_ASSERT_ERROR(m_next_cache_cntlr->operationPermissibleinCache(address, mem_op_type),
      "Tried to read from next-level cache, but data is already gone");
MYLOG("copyDataFromNextLevel l%d", m_mem_component);

   Byte data_buf[m_next_cache_cntlr->getCacheBlockSize()];
   m_next_cache_cntlr->retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD);

   CacheState::cstate_t cstate = mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED;

   // TODO: increment time? tag access on next level, also data access if this is not an upgrade

   if (modeled && !m_next_level_read_bandwidth.isInfinite())
   {
      SubsecondTime delay = m_next_level_read_bandwidth.getRoundedLatency(getCacheBlockSize() * 8);
      SubsecondTime t_done = m_master->m_next_level_read_bandwidth.getCompletionTime(t_now, delay);
      // Assume cache access time already contains transfer latency, increment time by contention delay only
      LOG_ASSERT_ERROR(t_done >= t_now + delay, "Did not expect next-level cache to be this fast");
      getMemoryManager()->incrElapsedTime(t_done - t_now - delay, ShmemPerfModel::_USER_THREAD);
   }


   // Insert the Cache Block in our own cache
   insertCacheBlock(address, cstate, data_buf, ShmemPerfModel::_USER_THREAD);
MYLOG("copyDataFromNextLevel l%d done", m_mem_component);
}


void
CacheCntlr::Prefetch(SubsecondTime t_now)
{
   IntPtr address_to_prefetch = INVALID_ADDRESS;

   {
      ScopedLock sl(getLock());

      if (m_master->m_prefetch_next <= t_now)
      {
         while(!m_master->m_prefetch_list.empty())
         {
            IntPtr address = m_master->m_prefetch_list.front();
            m_master->m_prefetch_list.pop_front();

            // Check address again, maybe some other core already brought it into the cache
            if (!operationPermissibleinCache(address, Core::READ))
            {
               address_to_prefetch = address;
               // Do at most one prefetch now, save the rest for a future call
               break;
            }
         }
      }
   }

   if (address_to_prefetch != INVALID_ADDRESS)
   {
      doPrefetch(address_to_prefetch, m_master->m_prefetch_next);
      atomic_add_subsecondtime(m_master->m_prefetch_next, PREFETCH_INTERVAL);
   }

   // In case the next-level cache has a prefetcher, run it
   if (m_next_cache_cntlr)
      m_next_cache_cntlr->Prefetch(t_now);
}

void
CacheCntlr::doPrefetch(IntPtr prefetch_address, SubsecondTime t_start)
{
   ++stats.prefetches;
   acquireStackLock(prefetch_address);
   MYLOG("prefetching %lx", prefetch_address);
   SubsecondTime t_before = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_start); // Start the prefetch at the same time as the original miss
   HitWhere::where_t hit_where = processShmemReqFromPrevCache(this, Core::READ, prefetch_address, true, Prefetch::OWN, t_start, false);

   if (hit_where == HitWhere::MISS)
   {
      /* last level miss, a message has been sent. */

      releaseStackLock(prefetch_address);
      waitForNetworkThread();
      wakeUpNetworkThread();

      hit_where = processShmemReqFromPrevCache(this, Core::READ, prefetch_address, false, Prefetch::OWN, t_start, false);

      LOG_ASSERT_ERROR(hit_where != HitWhere::MISS, "Line was not there after prefetch");
   }

   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_before); // Ignore changes to time made by the prefetch call
   releaseStackLock(prefetch_address);
}


/*****************************************************************************
 * operations called by cache on next-level cache
 *****************************************************************************/

HitWhere::where_t
CacheCntlr::processShmemReqFromPrevCache(CacheCntlr* requester, Core::mem_op_t mem_op_type, IntPtr address, bool modeled, Prefetch::prefetch_type_t isPrefetch, SubsecondTime t_issue, bool have_write_lock)
{
   #ifdef PRIVATE_L2_OPTIMIZATION
   bool have_write_lock_internal = have_write_lock;
   if (! have_write_lock && m_shared_cores > 1)
   {
      acquireStackLock(address, true);
      have_write_lock_internal = true;
   }
   #else
   bool have_write_lock_internal = true;
   #endif

   bool cache_hit = operationPermissibleinCache(address, mem_op_type), sibling_hit = false, prefetch_hit = false;
   bool first_hit = cache_hit;
   HitWhere::where_t hit_where = HitWhere::MISS;
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);

   if (!cache_hit && m_perfect)
   {
      cache_hit = true;
      hit_where = HitWhere::where_t(m_mem_component);
      if (cache_block_info)
         cache_block_info->setCState(CacheState::MODIFIED);
      else
         cache_block_info = insertCacheBlock(address, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, NULL, ShmemPerfModel::_USER_THREAD);
   }

   if (modeled && m_enabled)
   {
      ScopedLock sl(getLock());
      if (isPrefetch == Prefetch::NONE)
         getCache()->updateCounters(cache_hit);
      updateCounters(mem_op_type, address, cache_hit, getCacheState(address), isPrefetch);
   }

   if (cache_hit)
   {
      if (isPrefetch == Prefetch::NONE && cache_block_info->hasOption(CacheBlockInfo::PREFETCH))
      {
         // This line was fetched by the prefetcher and has proven useful
         stats.hits_prefetch++;
         prefetch_hit = true;
         cache_block_info->clearOption(CacheBlockInfo::PREFETCH);
      }

      // Increment Shared Mem Perf model cycle counts
      /****** TODO: for the last-level cache, this is also done by the network thread when the message comes in.
                    we probably shouldn't do this twice */
      /* TODO: if we end up getting the data from a sibling cache, the access time might be only that
         of the previous-level cache, not our (longer) access time */
      if (modeled)
      {
         ScopedLock sl(getLock());
         // This is a hit, but maybe the prefetcher filled it at a future time stamp. If so, delay.
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         if (m_master->mshr.count(address)
            && (m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now))
         {
            SubsecondTime latency = m_master->mshr[address].t_complete - t_now;
            stats.mshr_latency += latency;
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
         else
         {
            getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_USER_THREAD);
         }
      }

      if (mem_op_type != Core::READ)
      {
         /* Invalidate/flush in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
         {
            if (*it != requester)
            {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }
MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         assert(! cache_block_info->hasCachedLoc());
         #endif
      }
      else if (cache_block_info->getCState() == CacheState::MODIFIED)
      {
         /* Writeback in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
            if (*it != requester) {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }
MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
      }

      if (m_last_remote_hit_where != HitWhere::UNKNOWN)
      {
         // handleMsgFromDramDirectory just provided us with the data. Its source was left in m_last_remote_hit_where
         hit_where = m_last_remote_hit_where;
         m_last_remote_hit_where = HitWhere::UNKNOWN;
      }
      else
         hit_where = HitWhere::where_t(m_mem_component + (sibling_hit ? HitWhere::SIBLING : 0));

   }
   else
   {
      // Increment shared mem perf model cycle counts
      if (modeled)
         getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

      if (cache_block_info && (cache_block_info->getCState() == CacheState::SHARED))
      {
         /* Upgrade, invalidate in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
            if (*it != requester)
               latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::UPGRADE, NULL, ShmemPerfModel::_USER_THREAD).first);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         assert(! cache_block_info->hasCachedLoc());
         #endif
      }

      if (m_next_cache_cntlr)
      {
         /* upgrade: we'll get a new copy soon, invalidate the old, read-only copy */
         if (cache_block_info)
            invalidateCacheBlock(address);

         hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, address, modeled, isPrefetch == Prefetch::NONE ? Prefetch::NONE : Prefetch::OTHER, t_issue, have_write_lock_internal);
         if (hit_where != HitWhere::MISS)
         {
            cache_hit = true;
            /* get the data for ourselves */
            SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
            copyDataFromNextLevel(mem_op_type, address, modeled, t_now);
            if (isPrefetch != Prefetch::NONE)
               getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);
         }
      }
      else
      {
         if (m_master->m_dram_cntlr)
         {
            // Direct DRAM access
            cache_hit = true;
            if (cache_block_info)
            {
               // We already have the line: it must have been SHARED and this is a write (else there wouldn't have been a miss)
               // Upgrade silently
               cache_block_info->setCState(CacheState::MODIFIED);
               hit_where = HitWhere::where_t(m_mem_component);
            }
            else
            {
               Byte data_buf[getCacheBlockSize()];
               // Do the DRAM access and increment local time
               hit_where = accessDRAM(Core::READ, address, isPrefetch != Prefetch::NONE, data_buf);
               // Insert the line. Be sure to use SHARED/MODIFIED as appropriate (upgrades are free anyway), we don't want to have to write back clean lines
               insertCacheBlock(address, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, data_buf, ShmemPerfModel::_USER_THREAD);
               if (isPrefetch != Prefetch::NONE)
                  getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);
            }
         }
         else
         {
            initiateDirectoryAccess(mem_op_type, address, isPrefetch != Prefetch::NONE, t_issue);
         }
      }
   }

   if (cache_hit)
   {
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD);
      /* Store completion time so we can detect overlapping accesses */
      if (modeled && m_enabled && !first_hit)
      {
         ScopedLock sl(getLock());
         m_master->mshr[address] = make_mshr(t_issue, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
         cleanupMshr();
      }
   }

   if (modeled && m_master->m_prefetcher)
   {
      ScopedLock sl(getLock());

      // Always train the prefetcher
      std::vector<IntPtr> prefetchList = m_master->m_prefetcher->getNextAddress(address);

      // Only do prefetches on misses, or on hits to lines previously brought in by the prefetcher (if enabled)
      if (!cache_hit || (m_prefetch_on_prefetch_hit && prefetch_hit))
      {
         m_master->m_prefetch_list.clear();
         // Just talked to the next-level cache, wait a bit before we start to prefetch
         m_master->m_prefetch_next = t_issue + PREFETCH_INTERVAL;

         for(std::vector<IntPtr>::iterator it = prefetchList.begin(); it != prefetchList.end(); ++it)
         {
            // Keep at most PREFETCH_MAX_QUEUE_LENGTH entries in the prefetch queue
            if (m_master->m_prefetch_list.size() > PREFETCH_MAX_QUEUE_LENGTH)
               break;
            if (!operationPermissibleinCache(*it, Core::READ))
               m_master->m_prefetch_list.push_back(*it);
         }
      }
   }

   #ifdef PRIVATE_L2_OPTIMIZATION
   if (have_write_lock_internal && !have_write_lock)
   {
      releaseStackLock(address, true);
   }
   #else
   #endif

   MYLOG("returning %s", HitWhereString(hit_where));
   return hit_where;
}

void
CacheCntlr::notifyPrevLevelInsert(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address)
{
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   assert(cache_block_info);
   PrevCacheIndex idx = m_master->m_prev_cache_cntlrs.find(core_id, mem_component);
   cache_block_info->setCachedLoc(idx);
   #endif
}

void
CacheCntlr::notifyPrevLevelEvict(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address)
{
MYLOG("@%lx", address);
   if (m_master->m_evicting_buf && address == m_master->m_evicting_address) {
MYLOG("here being evicted");
   } else {
      #ifdef ENABLE_TRACK_SHARING_PREVCACHES
      SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
MYLOG("here in state %c", CStateString(getCacheState(address)));
      assert(cache_block_info);
      PrevCacheIndex idx = m_master->m_prev_cache_cntlrs.find(core_id, mem_component);
      cache_block_info->clearCachedLoc(idx);
      #endif
   }
}

HitWhere::where_t
CacheCntlr::accessDRAM(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, Byte* data_buf)
{
   ScopedLock sl(getLock()); // DRAM is shared and owned by m_master

   SubsecondTime t_issue = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   SubsecondTime dram_latency;
   HitWhere::where_t hit_where;

   switch (mem_op_type)
   {
      case Core::READ:
         boost::tie(dram_latency, hit_where) = m_master->m_dram_cntlr->getDataFromDram(address, m_core_id_master, data_buf, t_issue);
         break;

      case Core::READ_EX:
      case Core::WRITE:
         boost::tie(dram_latency, hit_where) = m_master->m_dram_cntlr->putDataToDram(address, m_core_id_master, data_buf, t_issue);
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type(%u)", mem_op_type);
   }

   // Increment local time with access latency
   getMemoryManager()->incrElapsedTime(dram_latency, ShmemPerfModel::_USER_THREAD);

   return hit_where;
}

void
CacheCntlr::initiateDirectoryAccess(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, SubsecondTime t_issue)
{
   bool exclusive = false;

   switch (mem_op_type)
   {
      case Core::READ:
         exclusive = false;
         break;

      case Core::READ_EX:
      case Core::WRITE:
         exclusive = true;
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type(%u)", mem_op_type);
   }

   bool first = false;
   {
      ScopedLock sl(getLock());
      CacheDirectoryWaiter* request = new CacheDirectoryWaiter(exclusive, isPrefetch, this, t_issue);
      m_master->m_directory_waiters.enqueue(address, request);
      if (m_master->m_directory_waiters.size(address) == 1)
         first = true;
   }

   if (first) {
      /* We're the first one to request this address, send the message to the directory now */
      if (exclusive)
         processExReqToDirectory(address);
      else
         processShReqToDirectory(address);
   } else {
MYLOG("%u previous waiters", m_master->m_directory_waiters.size(address));
   }
   /* else: someone else is busy with this cache line, they'll do everything for us */
}

void
CacheCntlr::processExReqToDirectory(IntPtr address)
{
   // We need to send a request to the Dram Directory Cache
MYLOG("EX REQ @ %lx", address);

   CacheState::cstate_t cstate = getCacheState(address);

   assert((cstate == CacheState::INVALID) || (cstate == CacheState::SHARED));
   if (cstate == CacheState::SHARED)
   {
      /* TODO: change this into one UPGRADE message (support will need to be added in the directory controller */
      // This will clear the 'Present' bit also
      invalidateCacheBlock(address);
      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
            m_core_id_master /* requester */,
            getHome(address) /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN, ShmemPerfModel::_USER_THREAD);
   }

   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, ShmemPerfModel::_USER_THREAD);
}

void
CacheCntlr::processShReqToDirectory(IntPtr address)
{
MYLOG("SH REQ @ %lx", address);
   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, ShmemPerfModel::_USER_THREAD);
}






/*****************************************************************************
 * internal operations called by cache on itself
 *****************************************************************************/

bool
CacheCntlr::operationPermissibleinCache(
      IntPtr address, Core::mem_op_t mem_op_type)
{
   // TODO: Verify why this works
   bool cache_hit = false;
   CacheState::cstate_t cstate = getCacheState(address);
MYLOG("address %lx state %c", address, CStateString(cstate));

   switch (mem_op_type)
   {
      case Core::READ:
         cache_hit = CacheState(cstate).readable();
         break;

      case Core::READ_EX:
      case Core::WRITE:
         cache_hit = CacheState(cstate).writable();
         break;

      default:
         LOG_PRINT_ERROR("Unsupported mem_op_type: %u", mem_op_type);
         break;
   }

   return cache_hit;
}


void
CacheCntlr::accessCache(
      Core::mem_op_t mem_op_type, IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length)
{
   switch (mem_op_type)
   {
      case Core::READ:
      case Core::READ_EX:
         m_master->m_cache->accessSingleLine(ca_address + offset, Cache::LOAD, data_buf, data_length,
                                             getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
         break;

      case Core::WRITE:
         m_master->m_cache->accessSingleLine(ca_address + offset, Cache::STORE, data_buf, data_length,
                                             getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
         // Write-through cache - Write the next level cache also
         if (m_cache_writethrough) {
            LOG_ASSERT_ERROR(m_next_cache_cntlr, "Writethrough enabled on last-level cache !?");
MYLOG("writethrough start");
            m_next_cache_cntlr->writeCacheBlock(ca_address, offset, data_buf, data_length, ShmemPerfModel::_USER_THREAD);
MYLOG("writethrough done");
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type: %u", mem_op_type);
         break;
   }
}




/*****************************************************************************
 * localized cache block operations
 *****************************************************************************/

SharedCacheBlockInfo*
CacheCntlr::getCacheBlockInfo(IntPtr address)
{
   return (SharedCacheBlockInfo*) m_master->m_cache->peekSingleLine(address);
}

CacheState::cstate_t
CacheCntlr::getCacheState(IntPtr address)
{
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   return (cache_block_info == NULL) ? CacheState::INVALID : cache_block_info->getCState();
}

SharedCacheBlockInfo*
CacheCntlr::setCacheState(IntPtr address, CacheState::cstate_t cstate)
{
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   cache_block_info->setCState(cstate);
   return cache_block_info;
}

void
CacheCntlr::invalidateCacheBlock(IntPtr address)
{
   __attribute__((unused)) CacheState::cstate_t old_cstate = getCacheState(address);
   assert(old_cstate != CacheState::MODIFIED);
   assert(old_cstate != CacheState::INVALID);

   m_master->m_cache->invalidateSingleLine(address);

   if (m_next_cache_cntlr)
      m_next_cache_cntlr->notifyPrevLevelEvict(m_core_id_master, m_mem_component, address);
}

void
CacheCntlr::retrieveCacheBlock(IntPtr address, Byte* data_buf, ShmemPerfModel::Thread_t thread_num)
{
   __attribute__((unused)) SharedCacheBlockInfo* cache_block_info = (SharedCacheBlockInfo*) m_master->m_cache->accessSingleLine(
      address, Cache::LOAD, data_buf, getCacheBlockSize(), getShmemPerfModel()->getElapsedTime(thread_num));
   LOG_ASSERT_ERROR(cache_block_info != NULL, "Expected block to be there but it wasn't");
}


/*****************************************************************************
 * cache block operations that update the previous level(s)
 *****************************************************************************/

SharedCacheBlockInfo*
CacheCntlr::insertCacheBlock(IntPtr address, CacheState::cstate_t cstate, Byte* data_buf, ShmemPerfModel::Thread_t thread_num)
{
MYLOG("insertCacheBlock l%d @ %lx as %c (now %c)", m_mem_component, address, CStateString(cstate), CStateString(getCacheState(address)));
   bool eviction;
   IntPtr evict_address;
   SharedCacheBlockInfo evict_block_info;
   Byte evict_buf[getCacheBlockSize()];

   LOG_ASSERT_ERROR(getCacheState(address) == CacheState::INVALID, "we already have this line, can't add it again");

   m_master->m_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         getShmemPerfModel()->getElapsedTime(thread_num));
   SharedCacheBlockInfo* cache_block_info = setCacheState(address, cstate);

   if (m_next_cache_cntlr && !m_perfect)
      m_next_cache_cntlr->notifyPrevLevelInsert(m_core_id_master, m_mem_component, address);
MYLOG("insertCacheBlock l%d local done", m_mem_component);


   if (eviction) {
MYLOG("evicting @%lx", evict_address);

      CacheState::cstate_t old_state = evict_block_info.getCState();
      {
         ScopedLock sl(getLock());
         transition(
            evict_address,
            Transition::EVICT,
            old_state,
            CacheState::INVALID
         );

         // Line was prefetched, but is evicted without ever being used
         if (evict_block_info.hasOption(CacheBlockInfo::PREFETCH))
            ++stats.evict_prefetch;

         if (old_state == CacheState::MODIFIED)
            ++stats.evict_modified;
         else
            ++stats.evict_shared;
      }

      /* TODO: this part looks a lot like updateCacheBlock's dirty case, but with the eviction buffer
         instead of an address, and with a message to the directory at the end. Merge? */

      LOG_PRINT("Eviction: addr(0x%x)", evict_address);
      if (! m_master->m_prev_cache_cntlrs.empty()) {
         ScopedLock sl(getLock());
         /* propagate the update to the previous levels. they will write modified data back to our evict buffer when needed */
         m_master->m_evicting_address = evict_address;
         m_master->m_evicting_buf = evict_buf;

         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
            latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(evict_address, CacheState::INVALID, Transition::EVICT_LOWER, NULL, thread_num).first);
         getMemoryManager()->incrElapsedTime(latency, thread_num);
         atomic_add_subsecondtime(stats.snoop_latency, latency);

         m_master->m_evicting_address = 0;
         m_master->m_evicting_buf = NULL;
      }

      /* now properly get rid of the evicted line */

      if (m_perfect)
      {
         // Nothing to do in this case
      }
      else if (!m_coherent)
      {
         // Don't notify the next level, it may have already evicted the line itself and won't like our notifyPrevLevelEvict
         // Make sure the line wasn't modified though (unless we're writethrough), else data would have been lost
         if (!m_cache_writethrough)
            LOG_ASSERT_ERROR(evict_block_info.getCState() != CacheState::MODIFIED, "Non-coherent cache is throwing away dirty data");
      }
      else if (m_next_cache_cntlr)
      {
         if (m_cache_writethrough) {
            /* If we're a write-through cache the new data is in the next level already */
         } else {
            /* Send dirty block to next level cache. Probably we have an evict/victim buffer to do that when we're idle, so ignore timing */
            if (evict_block_info.getCState() == CacheState::MODIFIED)
               m_next_cache_cntlr->writeCacheBlock(evict_address, 0, evict_buf, getCacheBlockSize(), thread_num);
         }
         m_next_cache_cntlr->notifyPrevLevelEvict(m_core_id_master, m_mem_component, evict_address);
      }
      else if (m_master->m_dram_cntlr)
      {
         if (evict_block_info.getCState() == CacheState::MODIFIED)
         {
            accessDRAM(Core::WRITE, evict_address, false, evict_buf);
         }
      }
      else
      {
         /* Send dirty block to directory */
         UInt32 home_node_id = getHome(evict_address);
         if (evict_block_info.getCState() == CacheState::MODIFIED)
         {
            // Send back the data also
MYLOG("evict FLUSH %lx", evict_address);
            getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REP,
                  MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
                  m_core_id /* requester */,
                  home_node_id /* receiver */,
                  evict_address,
                  evict_buf, getCacheBlockSize(),
                  HitWhere::UNKNOWN, thread_num);
         }
         else
         {
MYLOG("evict INV %lx", evict_address);
            LOG_ASSERT_ERROR(evict_block_info.getCState() == CacheState::SHARED,
                  "evict_address(0x%x), evict_state(%u)",
                  evict_address, evict_block_info.getCState());
            getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
                  MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
                  m_core_id /* requester */,
                  home_node_id /* receiver */,
                  evict_address,
                  NULL, 0,
                  HitWhere::UNKNOWN, thread_num);
         }
      }
MYLOG("insertCacheBlock l%d evict done", m_mem_component);
   }

   return cache_block_info;
MYLOG("insertCacheBlock l%d end", m_mem_component);
}

std::pair<SubsecondTime, bool>
CacheCntlr::updateCacheBlock(IntPtr address, CacheState::cstate_t new_cstate, Transition::reason_t reason, Byte* out_buf, ShmemPerfModel::Thread_t thread_num)
{
   LOG_ASSERT_ERROR(new_cstate < CacheState::NUM_CSTATE_STATES, "Invalid new cstate %u", new_cstate);

   /* first, propagate the update to the previous levels. they will write modified data back to us when needed */
   // TODO: performance fix: should only query those we know have the line (in cache_block_info->m_cached_locs)
   /* TODO: increment time (access tags) on previous-level caches, either all (for a snooping protocol that's not keeping track
              of cache_block_info->m_cached_locs) or selective (snoop filter / directory) */
   SubsecondTime latency = SubsecondTime::Zero();
   bool sibling_hit = false;
   if (! m_master->m_prev_cache_cntlrs.empty())
      for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
         std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(
            address, new_cstate, reason == Transition::EVICT ? Transition::EVICT_LOWER : reason, NULL, thread_num);
         // writeback_time is for the complete stack, so only model it at the last level, ignore latencies returned by previous ones
         //latency = getMax<SubsecondTime>(latency, res.first);
         sibling_hit |= res.second;
      }


   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
MYLOG("@%lx  %c > %c", address, CStateString(cache_block_info ? cache_block_info->getCState() : CacheState::INVALID), CStateString(new_cstate));

   bool buf_written = false, is_writeback = false;

   if (!cache_block_info) {
      /* We don't have the block, nothing to do */

   } else if (new_cstate == cache_block_info->getCState()) {
      /* We already have the right state, nothing to do */

   } else {
      {
         ScopedLock sl(getLock());
         transition(
            address,
            reason,
            getCacheState(address),
            new_cstate
         );
      }

      if (cache_block_info->getCState() == CacheState::MODIFIED) {
         /* data is modified, write it back */

         if (m_cache_writethrough) {
            /* next level already has the data */

         } else if (m_next_cache_cntlr) {
            /* write straight into the next level cache */
            Byte data_buf[getCacheBlockSize()];
            retrieveCacheBlock(address, data_buf, thread_num);
            m_next_cache_cntlr->writeCacheBlock(address, 0, data_buf, getCacheBlockSize(), thread_num);
            is_writeback = true;
            sibling_hit = true;

         } else if (out_buf) {
            /* someone (presumably the directory interfacing code) is waiting to consume the data */
            retrieveCacheBlock(address, out_buf, thread_num);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;

         } else {
            /* no-one will take my data !? */
            LOG_PRINT_ERROR("MODIFIED data is about to get lost!");

         }
         cache_block_info->setCState(CacheState::SHARED);
      }

      if (new_cstate == CacheState::INVALID && m_coherent) {
         invalidateCacheBlock(address);

      } else if (new_cstate == CacheState::SHARED) {
         cache_block_info->setCState(new_cstate);

      } else {
         LOG_ASSERT_ERROR(false, "Cannot upgrade block status to %d", new_cstate);

      }
   }
//MYLOG("@%lx  now %c", address, CStateString(getCacheState(address)));

   LOG_ASSERT_ERROR((getCacheState(address) == CacheState::INVALID) || (getCacheState(address) == new_cstate), "state didn't change as we wanted");
   LOG_ASSERT_ERROR(out_buf ? buf_written : true, "out_buf passed in but never written to");

   /* Assume tag access caused by snooping is already accounted for in lower level cache access time,
      so only when we accessed data should we return any latency */
   if (is_writeback)
      latency += m_writeback_time.getLatency();
   return std::pair<SubsecondTime, bool>(latency, sibling_hit);
}

void
CacheCntlr::writeCacheBlock(IntPtr address, UInt32 offset, Byte* data_buf, UInt32 data_length, ShmemPerfModel::Thread_t thread_num)
{
MYLOG(" ");

   // TODO: should we update access counter?

   if (m_master->m_evicting_buf && (address == m_master->m_evicting_address)) {
      MYLOG("writing to evict buffer");
assert(offset==0);
assert(data_length==getCacheBlockSize());
      if (data_buf)
         memcpy(m_master->m_evicting_buf + offset, data_buf, data_length);
   } else {
      __attribute__((unused)) SharedCacheBlockInfo* cache_block_info = (SharedCacheBlockInfo*) m_master->m_cache->accessSingleLine(
         address + offset, Cache::STORE, data_buf, data_length, getShmemPerfModel()->getElapsedTime(thread_num));
      LOG_ASSERT_ERROR(cache_block_info, "writethrough expected a hit at next-level cache but got miss");
      LOG_ASSERT_ERROR(cache_block_info->getCState() == CacheState::MODIFIED, "Got writeback for non-MODIFIED line");
   }

   if (m_cache_writethrough) {
      acquireStackLock(true);
      m_next_cache_cntlr->writeCacheBlock(address, offset, data_buf, data_length, thread_num);
      releaseStackLock(true);
   }
}



/*****************************************************************************
 * handle messages from directory (in network thread)
 *****************************************************************************/

void
CacheCntlr::handleMsgFromDramDirectory(
      core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   IntPtr address = shmem_msg->getAddress();

   acquireStackLock(address);
MYLOG("begin");

   switch (shmem_msg_type)
   {
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP:
MYLOG("EX REP<%u @ %lx", sender, address);
         processExRepFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP:
MYLOG("SH REP<%u @ %lx", sender, address);
         processShRepFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REQ:
MYLOG("INV REQ<%u @ %lx", sender, address);
         processInvReqFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REQ:
MYLOG("FLUSH REQ<%u @ %lx", sender, address);
         processFlushReqFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::WB_REQ:
MYLOG("WB REQ<%u @ %lx", sender, address);
         processWbReqFromDramDirectory(sender, shmem_msg);
         break;
      default:
         LOG_PRINT_ERROR("Unrecognized msg type: %u", shmem_msg_type);
         break;
   }

   if ((shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP) || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP))
   {
      getLock().acquire(); // Keep lock when handling m_directory_waiters
      while(! m_master->m_directory_waiters.empty(address)) {
         CacheDirectoryWaiter* request = m_master->m_directory_waiters.front(address);
         getLock().release();

         if (request->exclusive && (getCacheState(address) != CacheState::MODIFIED)) {
MYLOG("have SHARED, upgrading to MODIFIED for #%u", request->cache_cntlr->m_core_id);

            /* Upgrade, invalidate in previous levels */
            /* (Don't invalidate the block here, as processExReqToDirectory() needs to see
                the SHARED state so it knows also to send an INV_REP message */
            // TODO: only those in cache_block_info->m_cached_locs
            SubsecondTime latency = SubsecondTime::Zero();
            for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
               latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(
                  address, CacheState::INVALID, Transition::UPGRADE, NULL, ShmemPerfModel::_SIM_THREAD).first);
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_SIM_THREAD);
            atomic_add_subsecondtime(stats.snoop_latency, latency);

            processExReqToDirectory(address);

            releaseStackLock(address);
            return;
         }

         if (request->isPrefetch)
            getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);

         // Set the Counters in the Shmem Perf model accordingly
         // Set the counter value in the USER thread to that in the SIM thread
         SubsecondTime t_core = request->cache_cntlr->getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD),
                       t_here = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);
         if (t_here > t_core) {
MYLOG("adjusting time in #%u from %lu to %lu", request->cache_cntlr->m_core_id, t_core.getNS(), t_here.getNS());
            /* Unless the requesting core is already ahead of us (it may very well be if this cache's master thread's cpu
               is falling behind), update its time */
            // TODO: update master thread time in initiateDirectoryAccess ?
            request->cache_cntlr->getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_here);
         }

MYLOG("wakeup user #%u", request->cache_cntlr->m_core_id);
         request->cache_cntlr->m_last_remote_hit_where = shmem_msg->getWhere();

         //releaseStackLock(address);
         // Pass stack lock through to user thread
         wakeUpUserThread(request->cache_cntlr->m_user_thread_sem);
         waitForUserThread(request->cache_cntlr->m_network_thread_sem);
         acquireStackLock(address);

         {
            ScopedLock sl(request->cache_cntlr->getLock());
            request->cache_cntlr->m_master->mshr[address] = make_mshr(request->t_issue, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));
            cleanupMshr();
         }

         getLock().acquire();
         m_master->m_directory_waiters.dequeue(address);
         delete request;
      }
      getLock().release();
MYLOG("woke up all");
   }

   releaseStackLock(address);

MYLOG("done");
}

void
CacheCntlr::processExRepFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   // Forward data from message to LLC, don't incur LLC data access time (writeback will be done asynchronously)
   //getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
MYLOG("processExRepFromDramDirectory l%d", m_mem_component);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   insertCacheBlock(address, CacheState::MODIFIED, data_buf, ShmemPerfModel::_SIM_THREAD);
MYLOG("processExRepFromDramDirectory l%d end", m_mem_component);
}

void
CacheCntlr::processShRepFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   // Forward data from message to LLC, don't incur LLC data access time (writeback will be done asynchronously)
   //getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
MYLOG("processShRepFromDramDirectory l%d", m_mem_component);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   // Insert Cache Block in L2 Cache
   insertCacheBlock(address, CacheState::SHARED, data_buf, ShmemPerfModel::_SIM_THREAD);
}

void
CacheCntlr::processInvReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processInvReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::SHARED);

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);

      updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, NULL, ShmemPerfModel::_SIM_THREAD);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}

void
CacheCntlr::processFlushReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processFlushReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::MODIFIED);

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_SIM_THREAD);

      // Flush the line
      Byte data_buf[getCacheBlockSize()];
      updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, data_buf, ShmemPerfModel::_SIM_THREAD);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}

void
CacheCntlr::processWbReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processWbReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::MODIFIED);

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_SIM_THREAD);


      // Write-Back the line
      Byte data_buf[getCacheBlockSize()];
      updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, data_buf, ShmemPerfModel::_SIM_THREAD);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::WB_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::DRAM_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}



/*****************************************************************************
 * statistics functions
 *****************************************************************************/

void
CacheCntlr::updateCounters(Core::mem_op_t mem_op_type, IntPtr address, bool cache_hit, CacheState::cstate_t state, Prefetch::prefetch_type_t isPrefetch)
{
   /* If another miss to this cache line is still in progress:
      operationPermissibleinCache() will think it's a hit (so cache_hit == true) since the processing
      of the previous miss was done instantaneously. But mshr[address] contains its completion time */
   SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   bool overlapping = m_master->mshr.count(address) && m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now;

   if (mem_op_type == Core::WRITE)
   {
      if (isPrefetch != Prefetch::NONE)
         stats.stores_prefetch++;
      if (isPrefetch != Prefetch::OWN)
      {
         stats.stores++;
         stats.stores_state[state]++;
         if (! cache_hit || overlapping) {
            stats.store_misses++;
            stats.store_misses_state[state]++;
            if (overlapping) stats.store_overlapping_misses++;
         }
      }
   }
   else
   {
      if (isPrefetch != Prefetch::NONE)
         stats.loads_prefetch++;
      if (isPrefetch != Prefetch::OWN)
      {
         stats.loads++;
         stats.loads_state[state]++;
         if (! cache_hit) {
            stats.load_misses++;
            stats.load_misses_state[state]++;
            if (overlapping) stats.load_overlapping_misses++;
         }
      }
   }

   cleanupMshr();

   #ifdef ENABLE_TRANSITIONS
   transition(
      address,
      mem_op_type == Core::WRITE ? Transition::CORE_WR : (mem_op_type == Core::READ_EX ? Transition::CORE_RDEX : Transition::CORE_RD),
      state,
      mem_op_type == Core::READ && state != CacheState::MODIFIED ? CacheState::SHARED : CacheState::MODIFIED
   );
   #endif
}

void
CacheCntlr::cleanupMshr()
{
   /* Keep only last 8 MSHR entries */
   while(m_master->mshr.size() > 8) {
      IntPtr address_min = 0;
      SubsecondTime time_min = SubsecondTime::MaxTime();
      for(Mshr::iterator it = m_master->mshr.begin(); it != m_master->mshr.end(); ++it) {
         if (it->second.t_complete < time_min) {
            address_min = it->first;
            time_min = it->second.t_complete;
         }
      }
      m_master->mshr.erase(address_min);
   }
}

void
CacheCntlr::transition(IntPtr address, Transition::reason_t reason, CacheState::cstate_t old_state, CacheState::cstate_t new_state)
{
#ifdef ENABLE_TRANSITIONS
   if (m_enabled) {
      stats.transitions[old_state][new_state]++;
      if (old_state == CacheState::INVALID) {
         if (seen.count(address) == 0)
            old_state = CacheState::INVALID_COLD;
         else if (seen[address] == Transition::EVICT || seen[address] == Transition::EVICT_LOWER)
            old_state = CacheState::INVALID_EVICT;
         else if (seen[address] == Transition::COHERENCY)
            old_state = CacheState::INVALID_COHERENCY;
      }
      stats.transition_reasons[reason][old_state][new_state]++;
   }
   seen[address] = reason;
#endif
}


/*****************************************************************************
 * utility functions
 *****************************************************************************/

/* Lock design:
   - we want to allow concurrent access for operations that only touch the first-level cache
   - we want concurrent access to different addresses as much as possible

   Master last-level cache contains one shared/exclusive-lock per set (according to the first-level cache's set size)
   - First-level cache transactions acquire the lock pertaining to the set they'll use in shared mode.
     Multiple first-level caches can do this simultaneously.
     Since only a single thread accesses each L1, there should be no extra per-cache lock needed
     (The above is not strictly true, but Core takes care of this since MemoryManager only has one cycle count anyway).
     On a miss, a lock upgrade is needed.
   - Other levels, or the first level on miss, acquire the lock in exclusive mode which locks out both L1-only and L2+ transactions.
   #ifdef PRIVATE_L2_OPTIMIZATION
   - (On Nehalem, the L2 is private so it is only the L3 (the first level with m_sharing_cores > 1) that takes the exclusive lock).
   #endif

   Additionally, for per-cache objects that are not private to a cache set, each cache controller has its own (normal) lock,
   use getLock() for this. This is required for statistics updates, the directory waiters queue, etc.
*/

void
CacheCntlr::acquireLock(UInt64 address)
{
MYLOG("cache lock acquire %u # %u @ %lx", m_mem_component, m_core_id, address);
   assert(isFirstLevel());
   // Lock this L1 cache for the set containing <address>.
   lastLevelCache()->m_master->getSetLock(address)->acquire_shared(m_core_id);
}

void
CacheCntlr::releaseLock(UInt64 address)
{
MYLOG("cache lock release %u # %u @ %lx", m_mem_component, m_core_id, address);
   assert(isFirstLevel());
   lastLevelCache()->m_master->getSetLock(address)->release_shared(m_core_id);
}

void
CacheCntlr::acquireStackLock(UInt64 address, bool this_is_locked)
{
MYLOG("stack lock acquire %u # %u @ %lx", m_mem_component, m_core_id, address);
   // Lock the complete stack for the set containing <address>
   if (this_is_locked)
      // If two threads decide to upgrade at the same time, we could deadlock.
      // Upgrade therefore internally releases the cache lock!
      lastLevelCache()->m_master->getSetLock(address)->upgrade(m_core_id);
   else
      lastLevelCache()->m_master->getSetLock(address)->acquire_exclusive();
}

void
CacheCntlr::releaseStackLock(UInt64 address, bool this_is_locked)
{
MYLOG("stack lock release %u # %u @ %lx", m_mem_component, m_core_id, address);
   if (this_is_locked)
      lastLevelCache()->m_master->getSetLock(address)->downgrade(m_core_id);
   else
      lastLevelCache()->m_master->getSetLock(address)->release_exclusive();
}


CacheCntlr*
CacheCntlr::lastLevelCache()
{
   if (! m_last_level) {
      /* Find last-level cache */
      CacheCntlr* last_level = this;
      while(last_level->m_next_cache_cntlr)
         last_level = last_level->m_next_cache_cntlr;
      m_last_level = last_level;
   }
   return m_last_level;
}

bool
CacheCntlr::isShared(core_id_t core_id)
{
   core_id_t core_id_master = core_id - core_id % m_shared_cores;
   return core_id_master == m_core_id_master;
}


/*** threads ***/

void
CacheCntlr::wakeUpUserThread(Semaphore* user_thread_sem)
{
   (user_thread_sem ? user_thread_sem : m_user_thread_sem)->signal();
}

void
CacheCntlr::waitForUserThread(Semaphore* network_thread_sem)
{
   (network_thread_sem ? network_thread_sem : m_network_thread_sem)->wait();
}

void
CacheCntlr::waitForNetworkThread()
{
   m_user_thread_sem->wait();
}

void
CacheCntlr::wakeUpNetworkThread()
{
   m_network_thread_sem->signal();
}

Semaphore*
CacheCntlr::getUserThreadSemaphore()
{
   return m_user_thread_sem;
}

Semaphore*
CacheCntlr::getNetworkThreadSemaphore()
{
   return m_network_thread_sem;
}

}
