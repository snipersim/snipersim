#include "simulator.h"
#include "cache.h"
#include "log.h"
#include "stats.h"
#include "config.hpp"
#include <memory>
// #define DEBUG_TLB
// Cache class
// constructors/destructors

char CStateString(CacheState::cstate_t cstate)
{
   switch (cstate)
   {
   case CacheState::INVALID:
      return 'I';
   case CacheState::SHARED:
      return 'S';
   case CacheState::SHARED_UPGRADING:
      return 'u';
   case CacheState::MODIFIED:
      return 'M';
   case CacheState::EXCLUSIVE:
      return 'E';
   case CacheState::OWNED:
      return 'O';
   case CacheState::INVALID_COLD:
      return '_';
   case CacheState::INVALID_EVICT:
      return 'e';
   case CacheState::INVALID_COHERENCY:
      return 'c';
   default:
      return '?';
   }
}

int bits_set(uint8_t x)
{

   return __builtin_popcount(x);
}
float average_data_reuse;
float average_metadata_reuse;
float average_tlb_reuse;

UInt64 sum_data_reuse;
UInt64 sum_data_utilization;

UInt64 sum_metadata_reuse;
UInt64 sum_metadata_utilization;

UInt64 sum_tlb_reuse;
UInt64 sum_tlb_utilization;

UInt64 number_of_data_reuse;
UInt64 number_of_metadata_reuse;
UInt64 number_of_tlb_reuse;

Cache::Cache(
    String name,
    String cfgname,
    core_id_t core_id,
    UInt32 num_sets,
    UInt32 associativity,
    UInt32 cache_block_size,
    String replacement_policy,
    cache_t cache_type,
    hash_t hash,
    FaultInjector *fault_injector,
    AddressHomeLookup *ahl,
    bool is_tlb, int *page_size, int number_of_page_sizes)
    : CacheBase(name, num_sets, associativity, cache_block_size, hash, ahl),
      m_enabled(false),
      m_num_accesses(0),
      m_num_hits(0),
      m_cache_type(cache_type),
      m_fault_injector(fault_injector),
      m_pagesizes(NULL),
      m_number_of_page_sizes(number_of_page_sizes),
      average_data_reuse(0),
      average_metadata_reuse(0),
      average_tlb_reuse(0),
      sum_data_reuse(0),
      sum_data_utilization(0),
      sum_metadata_reuse(0),
      sum_metadata_utilization(0),
      sum_tlb_reuse(0),
      sum_tlb_utilization(0),
      number_of_data_reuse(0),
      number_of_metadata_reuse(0),
      number_of_tlb_reuse(0),
      metadata_passthrough_loc(Sim()->getCfg()->getInt("perf_model/metadata/passthrough_loc"))
{

   // Use of unique_ptr to avoid memory leaks
   m_pagesizes = std::unique_ptr<int[]>(new int[m_number_of_page_sizes]);
   for (int i = 0; i < m_number_of_page_sizes; i++)
   {
      m_pagesizes[i] = page_size[i];
   }

   reuse_levels[0] = 5; // kanellok Fix: these thresholds should be configurable
   reuse_levels[1] = 10;
   reuse_levels[2] = 20;

   m_set_info = CacheSet::createCacheSetInfo(name, cfgname, core_id, replacement_policy, m_associativity);
   m_sets = new CacheSet *[m_num_sets];
   m_fake_sets = new CacheSet *[1]; // kanellok  Hack: create a fake set for L2 cache to store metadata and "emulate" metadata bypassing

   if (!is_tlb)
      std::cout << "[Memory Hierarchy] Creating " << name << " cache with " << m_num_sets << " sets, " << associativity << "-way associative, " << m_blocksize << "B blocksize" << std::endl;

   for (UInt32 i = 0; i < m_num_sets; i++)
   {
      m_sets[i] = CacheSet::createCacheSet(cfgname, core_id, replacement_policy, m_cache_type, m_associativity, m_blocksize, m_set_info);
   }

   if (name == "L2" && metadata_passthrough_loc > 2)
   {
      std::cout << "Creating one fake set for L2 cache since metadata cant be cached in L2" << std::endl;
      m_fake_sets[0] = CacheSet::createCacheSet(cfgname, core_id, replacement_policy, m_cache_type, 1, m_blocksize, m_set_info);
   }

#ifdef ENABLE_SET_USAGE_HIST
   m_set_usage_hist = new UInt64[m_num_sets];
   for (UInt32 i = 0; i < m_num_sets; i++)
      m_set_usage_hist[i] = 0;
#endif

   m_page_walk_cacheblocks.clear();
   m_is_tlb = true;

   for (int i = 0; i < 5; i++)
   {

      registerStatsMetric(name, core_id, String("data-reuse-") + std::to_string(i).c_str(), &data_reuse[i]);
   }
   for (int i = 0; i < 5; i++)
   {

      registerStatsMetric(name, core_id, String("metadata-reuse-") + std::to_string(i).c_str(), &metadata_reuse[i]);
   }

   for (int i = 0; i < 5; i++)
   {

      registerStatsMetric(name, core_id, String("tlb-reuse-") + std::to_string(i).c_str(), &tlb_reuse[i]);
   }

   for (int i = 0; i < 8; i++)
   {

      registerStatsMetric(name, core_id, String("data-util-") + std::to_string(i).c_str(), &data_util[i]);
   }

   for (int i = 0; i < 8; i++)
   {

      registerStatsMetric(name, core_id, String("metadata-util-") + std::to_string(i).c_str(), &metadata_util[i]);
   }

   for (int i = 0; i < 8; i++)
   {

      registerStatsMetric(name, core_id, String("tlb-util-") + std::to_string(i).c_str(), &tlb_util[i]);
   }

   // registerStatsMetric(name, core_id, String("average_data_reuse"), &average_data_reuse);
   // registerStatsMetric(name, core_id, String("average_metadata_reuse"), &average_metadata_reuse);
   // registerStatsMetric(name, core_id, String("average_tlb_reuse"), &average_tlb_reuse);
}

Cache::~Cache()
{

#ifdef ENABLE_SET_USAGE_HIST
   printf("Cache %s set usage:", m_name.c_str());
   for (SInt32 i = 0; i < (SInt32)m_num_sets; i++)
      printf(" %" PRId64, m_set_usage_hist[i]);
   printf("\n");
   delete[] m_set_usage_hist;
#endif

   if (m_set_info)
      delete m_set_info;

   for (SInt32 i = 0; i < (SInt32)m_num_sets; i++)
      delete m_sets[i];
   delete[] m_sets;
   if (m_name == "L2" && metadata_passthrough_loc > 2)
   {
      for (SInt32 i = 0; i < 1; i++)
         delete m_fake_sets[i];
      delete[] m_fake_sets;
   }
}

Lock &
Cache::getSetLock(IntPtr addr)
{
   IntPtr tag;
   UInt32 set_index;

   splitAddress(addr, tag, set_index);
   assert(set_index < m_num_sets);

   return m_sets[set_index]->getLock();
}

bool Cache::invalidateSingleLine(IntPtr addr)
{
   IntPtr tag;
   UInt32 set_index;
   bool result = false;
   bool fake_result = false;

   splitAddress(addr, tag, set_index);
   assert(set_index < m_num_sets);

   if (m_name == "L2" && metadata_passthrough_loc > 2)
   {
      splitAddress(addr, tag, set_index);
      fake_result = m_fake_sets[0]->invalidate(tag);
   }

   result = m_sets[set_index]->invalidate(tag);

   return result || fake_result;
}

CacheBlockInfo *
Cache::accessSingleLine(IntPtr addr, access_t access_type,
                        Byte *buff, UInt32 bytes, SubsecondTime now, bool update_replacement, bool tlb_entry, bool is_metadata)
{
   // assert((buff == NULL) == (bytes == 0));

   IntPtr tag;
   UInt32 set_index;
   UInt32 line_index = 0;
   UInt32 block_offset;
   splitAddress(addr, tag, set_index, block_offset);

   CacheSet *set = m_sets[set_index];
   CacheBlockInfo *cache_block_info = set->find(tag, &line_index);

   if (cache_block_info == NULL)
   {
      if (m_name == "L2" && metadata_passthrough_loc > 2)
      {
         set = m_fake_sets[0];
         cache_block_info = set->find(tag, &line_index);
      }
      if (cache_block_info == NULL)
         return NULL;
   }

   if (tlb_entry && !(cache_block_info->isTLBBlock()))
      return NULL;

   if (access_type == LOAD)
   {
      // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into buff instead
      if (m_fault_injector)
         m_fault_injector->preRead(addr, set_index * m_associativity + line_index, bytes, (Byte *)m_sets[set_index]->getDataPtr(line_index, block_offset), now);

      set->read_line(line_index, block_offset, buff, bytes, update_replacement);
   }
   else
   {
      set->write_line(line_index, block_offset, buff, bytes, update_replacement);

      // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into buff instead
      if (m_fault_injector)
         m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, bytes, (Byte *)m_sets[set_index]->getDataPtr(line_index, block_offset), now);
   }

   return cache_block_info;
}

void Cache::insertSingleLine(IntPtr addr, Byte *fill_buff,
                             bool *eviction, IntPtr *evict_addr,
                             CacheBlockInfo *evict_block_info, Byte *evict_buff,
                             SubsecondTime now, CacheCntlr *cntlr,
                             CacheBlockInfo::block_type_t btype)
{
   IntPtr tag;
   UInt32 set_index;
   splitAddress(addr, tag, set_index);

   CacheBlockInfo *cache_block_info = CacheBlockInfo::create(m_cache_type);
   cache_block_info->setTag(tag);
   cache_block_info->setBlockType(btype);

   bool metadata_request = (btype == CacheBlockInfo::block_type_t::PAGE_TABLE) ||
                           (btype == CacheBlockInfo::block_type_t::PAGE_TABLE_PASSTHROUGH) ||
                           (btype == CacheBlockInfo::block_type_t::SECURITY) ||
                           (btype == CacheBlockInfo::block_type_t::EXPRESSIVE) ||
                           (btype == CacheBlockInfo::block_type_t::TLB_ENTRY) ||
                           (btype == CacheBlockInfo::block_type_t::TLB_ENTRY_PASSTHROUGH);

   if (m_name == "L2" && metadata_request && metadata_passthrough_loc > 2)
   {

      // std::cout << "[L2] Inserting in fake set of L2: " << addr <<std::endl;
      m_fake_sets[0]->insert(cache_block_info, fill_buff,
                             eviction, evict_block_info, evict_buff, cntlr);
   }
   else
   {
      m_sets[set_index]->insert(cache_block_info, fill_buff,
                                eviction, evict_block_info, evict_buff, cntlr);
   }

   *evict_addr = tagToAddress(evict_block_info->getTag());

   if (m_name == "nuca-cache" && (evict_block_info->getBlockType() == CacheBlockInfo::TLB_ENTRY || evict_block_info->getBlockType() == CacheBlockInfo::TLB_ENTRY_PASSTHROUGH))
   {
      *eviction = false;
      evict_addr = NULL;
   }

   if ((*eviction) == true)
   {

      int reuse_value = evict_block_info->getReuse();

      if (evict_block_info->getBlockType() == CacheBlockInfo::block_type_t::NON_PAGE_TABLE)
      {

         if (reuse_value == 0)
            data_reuse[0]++;
         else if (reuse_value <= reuse_levels[0])
            data_reuse[1]++;
         else if (reuse_value <= reuse_levels[1])
            data_reuse[2]++;
         else if (reuse_value <= reuse_levels[2])
            data_reuse[3]++;
         else
            data_reuse[4]++;

         sum_data_reuse += reuse_value;
         sum_data_utilization += evict_block_info->getUsage();
         data_util[bits_set(evict_block_info->getUsage())]++;

         number_of_data_reuse++;
         average_data_reuse = sum_data_reuse / number_of_data_reuse;
      }
      else if (evict_block_info->getBlockType() == CacheBlockInfo::block_type_t::PAGE_TABLE)
      {
         if (reuse_value == 0)
            metadata_reuse[0]++;
         else if (reuse_value <= reuse_levels[0])
            metadata_reuse[1]++;
         else if (reuse_value <= reuse_levels[1])
            metadata_reuse[2]++;
         else if (reuse_value <= reuse_levels[2])
            metadata_reuse[3]++;
         else
            metadata_reuse[4]++;

         sum_metadata_reuse += reuse_value;
         sum_metadata_utilization += evict_block_info->getUsage();
         metadata_util[bits_set(evict_block_info->getUsage())]++;

         number_of_metadata_reuse++;
         average_metadata_reuse = sum_metadata_reuse / number_of_metadata_reuse;
      }
      else if (evict_block_info->getBlockType() == CacheBlockInfo::block_type_t::TLB_ENTRY || evict_block_info->getBlockType() == CacheBlockInfo::block_type_t::TLB_ENTRY_PASSTHROUGH)
      {

         if (reuse_value == 0)
            tlb_reuse[0]++;
         else if (reuse_value <= reuse_levels[0])
            tlb_reuse[1]++;
         else if (reuse_value <= reuse_levels[1])
            tlb_reuse[2]++;
         else if (reuse_value <= reuse_levels[2])
            tlb_reuse[3]++;
         else
            tlb_reuse[4]++;

         sum_tlb_reuse += reuse_value;
         sum_tlb_utilization += evict_block_info->getUsage();
         tlb_util[bits_set(evict_block_info->getUsage())]++;
         number_of_tlb_reuse++;
      }
   }

   if (m_fault_injector)
   {
      // NOTE: no callback is generated for read of evicted data
      UInt32 line_index = -1;
      __attribute__((unused)) CacheBlockInfo *res = m_sets[set_index]->find(tag, &line_index);
      LOG_ASSERT_ERROR(res != NULL, "Inserted line no longer there?");

      m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, m_sets[set_index]->getBlockSize(), (Byte *)m_sets[set_index]->getDataPtr(line_index, 0), now);
   }

#ifdef ENABLE_SET_USAGE_HIST
   ++m_set_usage_hist[set_index];
#endif

   delete cache_block_info;
}

CacheBlockInfo *
Cache::accessSingleLineTLB(IntPtr addr, access_t access_type,
                           Byte *buff, UInt32 bytes, SubsecondTime now, bool update_replacement)
{
   IntPtr tag;
   UInt32 set_index;
   UInt32 line_index = -1;
   UInt32 block_offset;
   CacheBlockInfo *cache_block_info = NULL;
   CacheSet *set = NULL;

   bool found_cache_block = false;

#ifdef DEBUG_TLB
   for (int page_size = 0; page_size < m_number_of_page_sizes; page_size++)
   {
      std::cout << "Testing page size = " << m_pagesizes[page_size];
   }
   std::cout << std::endl;
#endif

   for (int page_size = 0; page_size < m_number_of_page_sizes; page_size++)
   { // @kanellok iterate over all possible page sizes

#ifdef DEBUG_TLB
      std::cout << m_name << " lookup: Page size = " << m_pagesizes[page_size] << " Number of page sizes = " << m_number_of_page_sizes << std::endl;
#endif

      // splitAddressTLB(addr, tag, set_index, block_offset, m_pagesizes[page_size]); //@kanellok provide the page size to find the index
      // ksgoura  We are going to enable this line in the nect commit
#ifdef DEBUG_TLB
      std::cout << "Address =  " << addr << std::endl;
      std::cout << "Set index =  " << set_index << std::endl;
      std::cout << "Tag =  " << tag << std::endl;
      std::cout << std::endl;
#endif

      set = m_sets[set_index];
      cache_block_info = set->find(tag, &line_index);

      if (cache_block_info == NULL)
         continue;

      found_cache_block = true;

      if (access_type == LOAD)
      {
         // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into buff instead
         if (m_fault_injector)
            m_fault_injector->preRead(addr, set_index * m_associativity + line_index, bytes, (Byte *)m_sets[set_index]->getDataPtr(line_index, block_offset), now);

         set->read_line(line_index, block_offset, buff, bytes, update_replacement);
      }
      else
      {
         set->write_line(line_index, block_offset, buff, bytes, update_replacement);

         if (m_fault_injector)
            m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, bytes, (Byte *)m_sets[set_index]->getDataPtr(line_index, block_offset), now);
      }
      break;
   }

   if (found_cache_block)
      return cache_block_info;
   else
      return NULL;
}

void Cache::insertSingleLineTLB(IntPtr addr, Byte *fill_buff,
                                bool *eviction, IntPtr *evict_addr,
                                CacheBlockInfo *evict_block_info, Byte *evict_buff,
                                SubsecondTime now, CacheCntlr *cntlr,
                                CacheBlockInfo::block_type_t btype, int page_size, IntPtr ppn)
{
   IntPtr tag = 0;
   UInt32 set_index = 0;
   // splitAddressTLB(addr, tag, set_index, page_size);
// ksgoura We will uncomment this line in a future commit
#ifdef DEBUG_TLB
   std::cout << m_name << " insert: Page size = " << page_size << std::endl;
   std::cout << "Address =  " << addr << std::endl;
   std::cout << "Set index =  " << set_index << std::endl;
   std::cout << "Tag =  " << tag << std::endl;
#endif

   CacheBlockInfo *cache_block_info = CacheBlockInfo::create(m_cache_type);
   cache_block_info->setTag(tag);
   cache_block_info->setBlockType(btype);
   cache_block_info->setPageSize(page_size);
   cache_block_info->setPPN(ppn);
   m_sets[set_index]->insert(cache_block_info, fill_buff,
                             eviction, evict_block_info, evict_buff, cntlr);
   if (*eviction == true)
      page_size = evict_block_info->getPageSize();
   //*evict_addr = tagToAddressTLB(evict_block_info->getTag(), page_size);
   // ksgoura We will uncomment this line in a later commit

#ifdef DEBUG_TLB

   std::cout << "Inserted " << addr << " in set " << set_index << " with tag " << m_sets[set_index]->find(tag)->getTag() << " and page size " << m_sets[set_index]->find(tag)->getPageSize() << std::endl;
   if (*eviction == true)
      std::cout << "Evicted  address  " << *evict_addr << std::endl;
   std::cout << std::endl;

#endif

   if (m_fault_injector)
   {
      // NOTE: no callback is generated for read of evicted data
      UInt32 line_index = -1;
      __attribute__((unused)) CacheBlockInfo *res = m_sets[set_index]->find(tag, &line_index);
      LOG_ASSERT_ERROR(res != NULL, "Inserted line no longer there?");

      m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, m_sets[set_index]->getBlockSize(), (Byte *)m_sets[set_index]->getDataPtr(line_index, 0), now);
   }

#ifdef ENABLE_SET_USAGE_HIST
   ++m_set_usage_hist[set_index];
#endif

   delete cache_block_info;
}

// Single line cache access at addr
CacheBlockInfo *
Cache::peekSingleLine(IntPtr addr)
{
   IntPtr tag = 0;
   UInt32 set_index = 0;
   splitAddress(addr, tag, set_index);

   if (m_sets[set_index]->find(tag) == NULL)
   {

      if (m_name == "L2" && metadata_passthrough_loc > 2)
         return m_fake_sets[0]->find(tag);

      return NULL;
   }
   return m_sets[set_index]->find(tag);
}

void Cache::updateSetReplacement(IntPtr addr)
{
   IntPtr tag;
   UInt32 set_index;
   UInt32 line_index = -1;
   UInt32 block_offset;
   UInt32 bytes = 8;
   Byte *buff = NULL;

   splitAddress(addr, tag, set_index, block_offset);

   m_sets[set_index]->find(tag, &line_index);

   m_sets[set_index]->read_line(line_index, block_offset, buff, bytes, true);

   return;
}

void Cache::updateCounters(bool cache_hit)
{
   if (m_enabled)
   {
      m_num_accesses++;
      if (cache_hit)
         m_num_hits++;
   }
}

void Cache::updateHits(Core::mem_op_t mem_op_type, UInt64 hits)
{
   if (m_enabled)
   {
      m_num_accesses += hits;
      m_num_hits += hits;
   }
}

CacheSet *
Cache::getCacheSet(UInt32 set_index)
{
   return m_sets[set_index];
}

void Cache::measureStats()
{
   uint64_t accum = 0; /* accumulate stats over all sets */
   for (uint32_t set_index = 0; set_index < m_num_sets; ++set_index)
   {
      accum += m_sets[set_index]->countPageWalkCacheBlocks();
   }

   m_page_walk_cacheblocks.push_back(accum);

   accum = 0; /* accumulate stats over all sets */
   for (uint32_t set_index = 0; set_index < m_num_sets; ++set_index)
   {
      accum += m_sets[set_index]->countSecurityCacheBlocks();
   }

   m_security_cacheblocks.push_back(accum);

   accum = 0; /* accumulate stats over all sets */
   for (uint32_t set_index = 0; set_index < m_num_sets; ++set_index)
   {
      accum += m_sets[set_index]->countExpressiveCacheBlocks();
   }

   m_expressive_cacheblocks.push_back(accum);

   accum = 0; /* accumulate stats over all sets */
   for (uint32_t set_index = 0; set_index < m_num_sets; ++set_index)
   {
      accum += m_sets[set_index]->countUtopiaCacheBlocks();
   }
   m_utopia_cacheblocks.push_back(accum);

   accum = 0; /* accumulate stats over all sets */
   for (uint32_t set_index = 0; set_index < m_num_sets; ++set_index)
   {
      accum += m_sets[set_index]->countTLBCacheBlocks();
   }
   m_tlb_cacheblocks.push_back(accum);
}
void Cache::markMetadata(IntPtr address, CacheBlockInfo::block_type_t blocktype)
{

   IntPtr tag;
   UInt32 set_index;

   splitAddress(address, tag, set_index);

   for (UInt32 i = 0; i < getCacheSet(set_index)->getAssociativity(); i++)
   {

      if (peekBlock(set_index, i)->getTag() == tag)
      {
         peekBlock(set_index, i)->setBlockType(blocktype);
         break;
      }
   }
}
