#include "pwc.h"
#include "stats.h"
#include "config.hpp"
#include "simulator.h"
#include <cmath>
#include <iostream>
#include <utility>
#include "core_manager.h"
#include "cache_set.h"
#include "utopia.h"
#include "utils.h"
#include "hash_map_set.h"
#include "buddy_allocator.h"
#include "mimicos.h"



#define DEBUG

/*
 * The RestSeg class represents a "segment" of memory with a specialized mapping strategy.
 * It utilizes an internal cache structure (m_RestSeg_cache) to track which addresses (pages)
 * are part of this segment. 
 * 
 * - Each RestSeg has a certain size (in MB), page_size (in bits), associativity, 
 *   replacement policy, and hashing scheme.
 * - It also includes data structures to simulate permission filtering (SF) and tag arrays (TAR).
 * - The logs/statistics help track conflicts, hits, and overall usage.
 */

/**
 * @brief Constructs a RestSeg object, initializing its configuration and internal data structures.
 * 
 * @param _id Unique identifier for this RestSeg instance.
 * @param _size Size of the RestSeg in megabytes.
 * @param _page_size Page size used by the RestSeg (log2 of the actual page size in bytes).
 * @param _assoc Associativity of the RestSeg (number of ways).
 * @param _repl Replacement policy used by the RestSeg (e.g., LRU).
 * @param _hash Hashing scheme used for address mapping.
 * 
 * This constructor initializes the RestSeg object by setting up its configuration parameters,
 * creating necessary internal data structures, and allocating memory for permission filters
 * and tag arrays. It also registers various statistics metrics for tracking performance.
 */

RestSeg::RestSeg(int _id, int _size, int _page_size, int _assoc, String _repl, String _hash)
  : id(_id),
    size(_size),
    page_size(_page_size),
    assoc(_assoc),
    hash(_hash),
    repl(_repl),
    m_RestSeg_conflicts(0),
    m_RestSeg_accesses(0),
    m_RestSeg_hits(0)
{
  // Create a log file for this particular RestSeg, using the ID as a unique suffix.
  log_file_name = "RestSeg.log." + std::to_string(id);
  log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
  log_file.open(log_file_name.c_str());

  // This Lock object protects shared data (e.g., the internal cache) from concurrency issues.
  RestSeg_lock = new Lock();

  // num_sets = number of sets for this "cache-like" data structure
  // k_MEGA is presumably 1,048,576. We convert the size from MB and divide by (#ways * page_size).
  //  (1 << page_size) is the page size in bytes.
  num_sets = k_MEGA * size / (assoc * (1 << page_size)); // Number of sets in the RestSeg

#ifdef DEBUG
  std::cout << std::endl;
  std::cout << "[Utopia] Creating RestSeg with sets : " << num_sets 
            << " - page_size: " << page_size 
            << " - assoc: " << assoc << std::endl;
#endif

  /*
   * m_RestSeg_cache is the core structure that tracks which addresses belong to this RestSeg.
   *  - The "cache_block_size" is the page size (1 << page_size).
   *  - "repl" is the replacement policy (e.g., LRU), "hash" is the address-hashing scheme.
   */
  m_RestSeg_cache = new Cache(
    ("RestSeg_cache_" + std::to_string(id)).c_str(),  // Name for the cache
    "perf_model/utopia/RestSeg",                      // Config key
    0,                                                // Core ID (0 used here, but not always critical)
    num_sets,                                         // Number of sets
    assoc,                                            // Associativity
    (1L << page_size),                                // Line size (in bytes)
    repl,
    CacheBase::PR_L1_CACHE,
    CacheBase::parseAddressHash(hash)
  );

  // filter_size is used by the permission filter to track ownership bits
  filter_size = log2(assoc) + 1;

  // In the simplified model, 48 bits for addresses are assumed (x86-64 canonical). 
  // Subtract page_size and log2(num_sets), then convert to bytes. This is for TAR calculations.
  tag_size = (48 - page_size - ceil(log2(num_sets))) / 8;

  // TAR_size => total amount of memory for the Tag Array at all sets/ways
  TAR_size = tag_size * (num_sets * assoc) / 8;

  // SF_size => total amount of memory for the permission filter data structure
  SF_size = (num_sets * filter_size) / 8;

  int core_num = Config::getSingleton()->getTotalCores();

  /*
   * For each core, allocate space for:
   *   - permissions: the "permission filter" data structure
   *   - tags: the Tag Array
   * 
   * We store these base addresses in the 'permissions' and 'tags' vectors. 
   * Each core has its own region representing the "process" or "address space".
   */
  for (int i = 0; i < core_num; i++)
  {
#ifdef DEBUG
    std::cout << "[RestSeg:ID-" << i << "] "
              << "SF Size (KB): " << SF_size / 1024 << std::endl;

    std::cout << "[RestSeg-ID-" << i << "]"
              << " TAR entry size: " << tag_size << std::endl;

    std::cout << "[RestSeg-ID-" << i << "] "
              << "TAR Size (KB): " << TAR_size / 1024 << std::endl;
#endif

    // Allocate memory for "permission filter"
    char *RestSeg_per_base = (char *)malloc((num_sets * log2(assoc)) / 8);

    // Allocate memory for "tag array"
    char *RestSeg_tags_base = (char *)malloc(num_sets * (tag_size / 8));  

    // Store the addresses in the relevant vectors so they can be retrieved later
    permissions.push_back((IntPtr)RestSeg_per_base);
    tags.push_back((IntPtr)RestSeg_tags_base);
  }

  // Register some statistics that we will track: conflicts, hits, accesses, and allocations
  registerStatsMetric(("RestSeg_" + std::to_string(id)).c_str(), 0, "allocation_conflicts", &m_RestSeg_conflicts);
  registerStatsMetric(("RestSeg_" + std::to_string(id)).c_str(), 0, "hits", &m_RestSeg_hits);
  registerStatsMetric(("RestSeg_" + std::to_string(id)).c_str(), 0, "accesses", &m_RestSeg_accesses);
  registerStatsMetric(("RestSeg_" + std::to_string(id)).c_str(), 0, "allocations", &m_allocations);
}

/*
 * inRestSeg(...) checks whether a given address belongs to this RestSeg,
 * specifically for a particular app_id. The address is broken down into
 * set and tag via m_RestSeg_cache->splitAddress(). We then see if any valid
 * block in that set has a matching tag and the matching 'owner' (core_id + 1).
 *
 * Returns true if the address is found (with the correct owner), false otherwise.
 */
/**
 * @brief Checks if a given address is within the RestSeg cache and optionally counts statistics.
 *
 * This function checks if a given address is present in the RestSeg cache and whether it belongs to the correct core.
 * It also optionally increments access and hit statistics.
 *
 * @param address The address to check within the RestSeg cache.
 * @param count A boolean flag indicating whether to count statistics for this lookup.
 * @param now The current time, used for cache access timing.
 * @param core_id The ID of the core making the request.
 * @return True if the address is owned by the correct core, false otherwise.
 */
bool RestSeg::inRestSeg(IntPtr address, bool count, SubsecondTime now, int core_id)
{
#ifdef DEBUG
  log_file << std::endl;
  log_file << "--------------------------------" << std::endl;
#endif

  // Lock ensures thread-safety when checking or modifying the cache structure
  RestSeg_lock->acquire();

  // If we are counting stats for this lookup, increment the number of accesses
  if (count)
    m_RestSeg_accesses++;

  // Periodically, we can call track_utilization() to measure usage
  if (m_RestSeg_accesses % 10000 == 0)
  {
    track_utilization();
  }

  // We do a "Cache::LOAD" lookup in m_RestSeg_cache to see if it’s present at all
  bool hit = m_RestSeg_cache->accessSingleLine(address, Cache::LOAD, NULL, 0, now, true);

  // Break down the address into its cache tag and set index
  IntPtr tag;
  UInt32 set_index;
  m_RestSeg_cache->splitAddress(address, tag, set_index);

  bool owner_hit = false; // This specifically checks whether the block belongs to the correct app

#ifdef DEBUG
  log_file << "Checking if address: " << address << " is in RestSeg" << std::endl;
  log_file << "Tag: " << tag << std::endl;
  log_file << "Set index: " << set_index << std::endl;
#endif

  // We search within the set for a block with matching tag *and* matching owner
  for (UInt32 i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
  {
    if ((m_RestSeg_cache->peekBlock(set_index, i)->getTag() == tag) &&
        (m_RestSeg_cache->peekBlock(set_index, i)->getOwner() == (UInt64)(core_id + 1)))
    {
#ifdef DEBUG
      log_file << "It is probably inside way: " << i << std::endl;
#endif
      // For usage stats, we might increment a reuse counter
      m_RestSeg_cache->peekBlock(set_index, i)->increaseReuse();
      owner_hit = true;
      break;
    }
  }

#ifdef DEBUG
  log_file << "Owner hit: " << owner_hit << std::endl;
  log_file << "RestSeg hit: " << hit << std::endl;
#endif

  // If we're counting stats and the correct owner was found, increment hits
  if (count && owner_hit)
    m_RestSeg_hits++;

  RestSeg_lock->release();

  // The final returned value is specifically "did the correct core own this address"?
  return (owner_hit);
}

/*
 * Given an address, returns the "permission address" used by the permission filter
 * (SF). This address is effectively an offset into the permission array for 
 * a particular core (based on set_index).
 *
 * The RestSeg_cache is used only to get the set_index from the address;
 * filter_size * set_index indicates which location in 'permissions[core_id]' 
 * we should read or write.
 */
/**
 * @brief Calculates the permission address for a given memory address and core ID.
 *
 * This function computes the permission address by splitting the input address
 * into a tag and set index, and then using the set index to determine the offset
 * within the permissions array for the specified core.
 *
 * @param address The memory address for which the permission address is to be calculated.
 * @param core_id The ID of the core for which the permission address is to be calculated.
 * @return The calculated permission address as an IntPtr.
 */
IntPtr RestSeg::calculate_permission_address(IntPtr address, int core_id)
{
  IntPtr tag;
  UInt32 set_index;
  m_RestSeg_cache->splitAddress(address, tag, set_index);

#ifdef DEBUG
  log_file << std::endl;
  log_file << "--------------------------------" << std::endl;
  log_file << "Calculating permission address for address: " << address << std::endl;
  log_file << "Accessing permission " << permissions[core_id] + (set_index * filter_size) / 8 << std::endl;
  log_file << "Permissions base: " << permissions[core_id] << std::endl;
  log_file << "Set index: " << set_index << std::endl;
  log_file << "Filter size: " << filter_size << std::endl;
  log_file << "--------------------------------" << std::endl;
#endif

  // Return the pointer-like offset for this set
  return (IntPtr)(permissions[core_id] + (set_index * filter_size) / 8);
}

/*
 * Similar to calculate_permission_address, but for the Tag Array (TAR).
 * We again derive set_index from the address, then find which way in the set 
 * is correct. We then compute an offset (set_index * assoc + i) * tag_size 
 * into the 'tags[core_id]' array.
 *
 * Returns 0 if we did not find the address/owner combination in the set.
 */
/**
 * @brief Calculate the tag address for a given memory address and core ID.
 *
 * This function calculates the tag address for a given memory address and core ID
 * by checking each way in the cache set for a matching tag and owner. If a match
 * is found, it returns the calculated tag address. If no match is found, it returns 0.
 *
 * @param address The memory address for which the tag address is to be calculated.
 * @param core_id The ID of the core requesting the tag address.
 * @return The calculated tag address if a match is found, otherwise 0.
 */
IntPtr RestSeg::calculate_tag_address(IntPtr address, int core_id)
{
  IntPtr tag;
  UInt32 set_index;
  m_RestSeg_cache->splitAddress(address, tag, set_index);

  // Check each way for a matching tag and owner
  for (UInt32 i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
  {
    if ((m_RestSeg_cache->peekBlock(set_index, i)->getTag() == tag) &&
        (m_RestSeg_cache->peekBlock(set_index, i)->getOwner() == (UInt64)(core_id + 1)))
    {
#ifdef DEBUG
      log_file << std::endl;
      log_file << "--------------------------------" << std::endl;
      log_file << "Calculating tag address for address: " << address << std::endl;
      log_file << "Accessing tag " << tags[core_id] + (set_index * assoc + i) * tag_size << std::endl;
      log_file << "Tag base: " << tags[core_id] << std::endl;
      log_file << "Set index: " << set_index << std::endl;
      log_file << "Tag size: " << tag_size << std::endl;
      log_file << "--------------------------------" << std::endl;
#endif
      return (IntPtr)(tags[core_id] + (set_index * assoc + i) * tag_size);
    }
  }
  // Not found => 0
  return 0;
}

/*
 * permission_filter(...) checks if a particular set for 'address' is empty
 * of valid blocks (or at least empty of blocks owned by this core). If yes,
 * we can skip certain steps (like a tag match) as an optimization.
 *
 * Returns true if the set is empty, false otherwise.
 */
bool RestSeg::permission_filter(IntPtr address, int core_id)
{
  RestSeg_lock->acquire();

  IntPtr tag;
  UInt32 set_index;
  bool set_is_empty = true;

  m_RestSeg_cache->splitAddress(address, tag, set_index);

  // If any valid block in the set is owned by this core, then it's not empty
  for (UInt32 i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
  {
    if (m_RestSeg_cache->getCacheSet(set_index)->peekBlock(i)->isValid() &&
        m_RestSeg_cache->getCacheSet(set_index)->peekBlock(i)->getOwner() == (UInt64)(core_id + 1))
    {
      set_is_empty = false;
    }
  }

  RestSeg_lock->release();
  return set_is_empty;
}

/*
 * allocate(...) inserts 'address' into the RestSeg cache if there is capacity.
 * If forced == true, we attempt insertion regardless of capacity. 
 * If a line must be evicted, we return the evicted address so that 
 * the caller can handle it (e.g., place it in the FlexSeg).
 *
 * Returns a tuple: 
 *  (1) bool => whether the allocation succeeded,
 *  (2) bool => whether an eviction occurred,
 *  (3) IntPtr => the address that was evicted (if eviction==true).
 */
std::tuple<bool, bool, IntPtr> RestSeg::allocate(IntPtr address, SubsecondTime now, int core_id, bool forced)
{
#ifdef DEBUG
  log_file << std::endl;
  log_file << "---------------------------" << std::endl;
#endif

  RestSeg_lock->acquire();

  IntPtr tag;
  UInt32 set_index;
  bool eviction;
  IntPtr evict_addr;
  CacheBlockInfo evict_block_info;

#ifdef DEBUG
  log_file << "Allocating address: " << address 
           << " in RestSeg with page size: " << page_size << std::endl;
#endif

  // Track how many allocations we've made
  m_allocations++;

  // Determine the set index and tag for the address
  m_RestSeg_cache->splitAddress(address, tag, set_index);

#ifdef DEBUG
  log_file << "Checking if all ways are occupied" << std::endl;
#endif
  // We first check if the set has a free way, unless forced == true
  bool all_ways_occupied = true;
  for (UInt32 i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
  {
    if (!m_RestSeg_cache->peekBlock(set_index, i)->isValid())
    {
      all_ways_occupied = false;
      break;
    }
  }

#ifdef DEBUG
  log_file << "All ways are occupied: " << all_ways_occupied << std::endl;
#endif

  // If there is a free way or if forced is true, we can insert the line
  if (!all_ways_occupied || forced)
  {
    m_RestSeg_cache->insertSingleLine(address, NULL, &eviction, &evict_addr, &evict_block_info, NULL, now);
  }
  else
  {
#ifdef DEBUG
    log_file << "All ways are occupied, we cannot allocate the address" << std::endl;
#endif
    RestSeg_lock->release();
    return (std::make_tuple(false, false, static_cast<IntPtr>(-1)));
  }

#ifdef DEBUG
  log_file << "Adding address with set_index: " << set_index 
           << " and tag: " << tag << std::endl;
#endif

  /*
   * After insertion, we iterate over the set again to find which way we just inserted.
   * We set the "owner" field to (core_id + 1) so that inRestSeg(...) checks can
   * confirm ownership.
   */
  for (UInt32 i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
  {
    if (m_RestSeg_cache->peekBlock(set_index, i)->getTag() == tag &&
        m_RestSeg_cache->peekBlock(set_index, i)->getOwner() == 0)
    {
#ifdef DEBUG
      log_file << "Found where we inserted the block: in way " << i << std::endl;
#endif
      m_RestSeg_cache->peekBlock(set_index, i)->setOwner(core_id + 1);
      break;
    }
  }

#ifdef DEBUG
  log_file << " Caused eviction?: " << eviction << std::endl;
#endif

  // If an eviction occurred, increment the conflict counter
  if (eviction)
    m_RestSeg_conflicts++;

  RestSeg_lock->release();

#ifdef DEBUG
  log_file << "---------------------------" << std::endl;
#endif

  // Return (success, eviction, evicted_address)
  return (std::make_tuple(true, eviction, evict_addr));
}

/*
 * calculate_physical_address(...) determines the "physical page" within RestSeg 
 * that an address maps to. We derive which set and way it's in, then compute the 
 * base offset plus an index factor. The base_page_size is 4KB; factor = 2^(page_size - 12) 
 * helps scale up for larger pages if page_size > 12 bits.
 *
 * Returns -1 if the address is not found with matching ownership.
 */
IntPtr RestSeg::calculate_physical_address(IntPtr address, int core_id)
{
  IntPtr tag;
  UInt32 set_index;
  m_RestSeg_cache->splitAddress(address, tag, set_index);

  // Search for a matching block
  for (UInt32 i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
  {
    if (m_RestSeg_cache->peekBlock(set_index, i)->getTag() == tag &&
        m_RestSeg_cache->getCacheSet(set_index)->peekBlock(i)->getOwner() == (UInt64)(core_id + 1))
    {
      // base_page_size = 4KB
      int base_page_size = 12; // bits
      int factor = pow(2, page_size - base_page_size);

#ifdef DEBUG
      log_file << "---------------------------" << std::endl;
      log_file << "Finalized RSW for address: " << address << std::endl;
      log_file << "Calculating physical address for address: " << address << std::endl;
      log_file << "Set index: " << set_index << std::endl;
      log_file << "Tag: " << tag << std::endl;
      log_file << "RestSeg base: " << m_RestSeg_base << std::endl;
      log_file << "RestSeg page size: " << page_size << std::endl;
      log_file << "RestSeg assoc: " << assoc << std::endl;
      log_file << "RestSeg way: " << i << std::endl;
      log_file << "Physical page (4KB) granularity: " 
               << (IntPtr)(m_RestSeg_base + (set_index * assoc + i) * factor)
               << std::endl;
      log_file << "---------------------------" << std::endl;
#endif

      // The final physical frame (in 4KB units) is offset by factor
      return (IntPtr)(m_RestSeg_base + (set_index * assoc + i) * factor);
    }
  }

  // Not found
  return static_cast<IntPtr>(-1);
}

/*
 * track_utilization() counts how many valid blocks exist in the entire RestSeg cache
 * and pushes that count into the 'utilization' vector. 
 */
void RestSeg::track_utilization()
{
  int accum = 0;

  for (uint32_t set_index = 0; set_index < m_RestSeg_cache->getNumSets(); ++set_index)
  {
    for (uint32_t i = 0; i < m_RestSeg_cache->getCacheSet(set_index)->getAssociativity(); i++)
    {
      if (m_RestSeg_cache->getCacheSet(set_index)->peekBlock(i)->isValid())
        accum += 1;
    }
  }

  utilization.push_back(accum);
}

/*
 * Utopia class is the main "PhysicalMemoryAllocator" derivative that organizes 
 * memory allocations among multiple RestSeg objects and a Buddy allocator.
 * 
 *  - Each RestSeg is configured based on parameters from the config (size, page size, assoc).
 *  - If an address cannot be allocated in any RestSeg, it falls back to the buddy_allocator 
 *    ("FlexSeg" in some sense).
 */
Utopia::Utopia(String name, int memory_size, int max_order, int kernel_size, String frag_type)
  : PhysicalMemoryAllocator(name, memory_size, kernel_size)
{
  // Open a log file for Utopia
  log_file_name = "utopia.log";
  log_file.open(log_file_name);

  // The number of RestSeg segments is retrieved from the config
  RestSegs = Sim()->getCfg()->getInt("perf_model/utopia/RestSegs");

  // Two heuristic types (primary and secondary) can be used for advanced decisions
  heur_type_primary = (Utopia::utopia_heuristic)Sim()->getCfg()->getInt("perf_model/utopia/heuristic_primary");
  heur_type_secondary = (Utopia::utopia_heuristic)Sim()->getCfg()->getInt("perf_model/utopia/heuristic_secondary");

#ifdef DEBUG
  std::cout << std::endl;
  std::cout << "------ [MimicOS] U T O P I A -------" << std::endl;
  std::cout << std::endl;
  std::cout << "[UTOPIA] Heuristic primary  " << heur_type_primary << std::endl;
  std::cout << "[UTOPIA] Heuristic secondary  " << heur_type_secondary << std::endl;
#endif

  // TLB or PTE eviction thresholds might be used in some heuristics to trigger page table migration
  tlb_eviction_thr = (UInt32)(Sim()->getCfg()->getInt("perf_model/utopia/tlb_eviction_thr"));
  pte_eviction_thr = (UInt32)(Sim()->getCfg()->getInt("perf_model/utopia/pte_eviction_thr"));

#ifdef DEBUG
  std::cout << "[UTOPIA] TLB threshold:  " << tlb_eviction_thr << std::endl;
  std::cout << "[UTOPIA] PTE threshold:  " << pte_eviction_thr << std::endl;
#endif

  /*
   * Construct each RestSeg from config parameters: 
   *   - size, page_size, assoc, repl policy, hashing function
   * Then call set_base(...) to carve out the actual physical region. 
   * handle_page_table_allocations(...) is presumably for internal logic 
   * that manages where these segments are placed in the physical address space.
   */
  for (int i = 0; i < RestSegs; i++)
  {
    int RestSeg_size = Sim()->getCfg()->getIntArray("perf_model/utopia/RestSeg/size", i);
    int RestSeg_page_size = Sim()->getCfg()->getIntArray("perf_model/utopia/RestSeg/page_size", i);
    int RestSeg_assoc = Sim()->getCfg()->getIntArray("perf_model/utopia/RestSeg/assoc", i);
    String RestSeg_repl = Sim()->getCfg()->getStringArray("perf_model/utopia/RestSeg/repl", i);
    String RestSeg_hash = Sim()->getCfg()->getStringArray("perf_model/utopia/RestSeg/hash", i);

    RestSeg *RestSeg_object = new RestSeg(i + 1, RestSeg_size, RestSeg_page_size, RestSeg_assoc, RestSeg_repl, RestSeg_hash);

    // Set the base physical address for this RestSeg so we know where it begins
    RestSeg_object->set_base(handle_page_table_allocations(RestSeg_object->getSize() * 1024 * 1024));

    RestSeg_vector.push_back(RestSeg_object);
  }

  // Also construct a buddy allocator that manages the remainder of physical memory (a "FlexSeg")
  buddy_allocator = new Buddy(memory_size, max_order, kernel_size, frag_type);

#ifdef DEBUG
  std::cout << std::endl;
  std::cout << "------ [MimicOS] U T O P I A -------" << std::endl;
  std::cout << std::endl;
#endif
}

/*
 * allocate(...) tries to allocate the given 'address' of size 'size' in the RestSegs.
 * If all fail, it allocates in the buddy allocator (main memory, or "FlexSeg").
 *
 *  - If we succeed in a RestSeg, we record that fact in m_last_allocated_in_restseg.
 *  - If an eviction occurs in that RestSeg, the evicted address is allocated in the buddy allocator.
 * 
 * Returns a pair: (physical_address, page_size).
 */
std::pair<UInt64, UInt64> Utopia::allocate(UInt64 size, UInt64 address, UInt64 core_id, bool is_pagetable_allocation)
{
#ifdef DEBUG
  log_file << std::endl;
  log_file << "---------------------------" << std::endl;
  log_file << "Trying to to allocatie address: " << address << " in Utopia" << std::endl;
#endif
  SubsecondTime now = SubsecondTime::Zero();

  // Start from the last RestSeg and go backwards, or some policy. 
  // If the user wants a bigger page, it might be the last one, etc.
  for (int i = RestSegs - 1; i >= 0; i--)
  {
#ifdef DEBUG
    log_file << "Trying to allocate in RestSeg: " << i 
             << " with page size: " << RestSeg_vector[i]->getPageSize() << std::endl;
#endif
    auto allocation_result = RestSeg_vector[i]->allocate(address, now, core_id, false);

#ifdef DEBUG
    log_file << "Allocation result in RestSeg: " << std::get<0>(allocation_result) << std::endl;
    log_file << "Eviction from RestSeg: " << std::get<1>(allocation_result) << std::endl;
    log_file << "Evicted address in RestSeg: " << std::get<2>(allocation_result) << std::endl;
#endif

    if (std::get<0>(allocation_result))
    {
#ifdef DEBUG
      log_file << "Allocated in RestSeg: " << i << std::endl;
#endif
      m_last_allocated_in_restseg = true;

      // If an eviction occurred, we place that evicted address in the buddy allocator (FlexSeg)
      if (std::get<1>(allocation_result))
      {
#ifdef DEBUG
        log_file << "Evicted address: " << std::get<2>(allocation_result) << std::endl;
#endif
        auto ppn_flexseg = buddy_allocator->allocate(size, std::get<2>(allocation_result), core_id);

#ifdef DEBUG
        log_file << "Evicted address allocated in the FlexSeg: " << ppn_flexseg << std::endl;
#endif
      }

#ifdef DEBUG
      log_file << "Returning address: " 
               << RestSeg_vector[i]->calculate_physical_address(address, core_id) << std::endl;
#endif

      // Return (physical_address, actual_page_size_bits)
      return make_pair(RestSeg_vector[i]->calculate_physical_address(address, core_id),
                       RestSeg_vector[i]->getPageSize());
    }
  }

#ifdef DEBUG
  log_file << "Could not allocate in RestSegs" << std::endl;
#endif

  // If all RestSeg allocations fail, we allocate in the buddy allocator
  m_last_allocated_in_restseg = false;

#ifdef DEBUG
  log_file << "Allocating in the FlexSeg" << std::endl;
#endif

  IntPtr page = buddy_allocator->allocate(size, address, core_id);
  // The buddy allocator uses 4KB pages, hence 12 bits 
  return make_pair(page, 12);
}


IntPtr Utopia::migratePage(IntPtr address, IntPtr ppn, int page_size, int app_id)
{
  // get the corresponding page table 

  #ifdef DEBUG
    log_file << std::endl;
    log_file << "---------------------------" << std::endl;
    log_file << "Migrating page: " << address << " to RestSeg" << std::endl;
  #endif

  // First, we delete the page from the page table
  Sim()->getMimicOS()->getPageTable(app_id)->deletePage(address);
  buddy_allocator->free(ppn,ppn);

  // Then, we allocate the page in the RestSeg, based on the page size
  for (int i = 0; i < RestSegs; i++)
  {
    if (RestSeg_vector[i]->getPageSize() == page_size)
    {
#ifdef DEBUG
      log_file << "Allocating in RestSeg: " << i << std::endl;
      log_file << "Page size: " << page_size << std::endl;
#endif
      auto allocation_result = RestSeg_vector[i]->allocate(address, SubsecondTime::Zero(), app_id, true);

#ifdef DEBUG
      log_file << "Allocation result in RestSeg: " << std::get<0>(allocation_result) << std::endl;
      log_file << "Eviction from RestSeg: " << std::get<1>(allocation_result) << std::endl;
      log_file << "Evicted address in RestSeg: " << std::get<2>(allocation_result) << std::endl;
#endif
      if (std::get<0>(allocation_result))
      {
        // If an eviction occurred, we place that evicted address in the buddy allocator (FlexSeg)
#ifdef DEBUG
        log_file << "Allocated in RestSeg: " << i << std::endl;
#endif
        if (std::get<1>(allocation_result))
        {
#ifdef DEBUG
          log_file << "Evicted address: " << std::get<2>(allocation_result) << std::endl;
#endif
          auto ppn_flexseg = buddy_allocator->allocate(pow(2, page_size), std::get<2>(allocation_result), app_id);
        
#ifdef DEBUG
          log_file << "Evicted address allocated in the FlexSeg: " << ppn_flexseg << std::endl;
#endif
          Sim()->getMimicOS()->getPageFaultHandler()->allocatePagetableFrames(address, app_id, ppn_flexseg, page_size, Sim()->getMimicOS()->getPageTable(app_id)->getMaxLevel());
          
        }
        log_file << "---------------------------" << std::endl;

        return RestSeg_vector[i]->calculate_physical_address(address, app_id);
      }
    }
  }

  return 0;
}



void Utopia::deallocate(UInt64 address, UInt64 core_id)
{
  // Not implemented or needed for now
  return;
}

/*
 * Used to artificially fragment memory in the buddy allocator.
 */
void Utopia::fragment_memory()
{
	buddy_allocator->fragmentMemory(Sim()->getCfg()->getFloat("perf_model/" + m_name + "/target_fragmentation"));
  return;
}

/*
 * allocate_ranges(...) can be extended to allocate multiple ranges from 
 * either RestSeg or buddy allocator. Currently, it returns an empty vector.
 */
std::vector<Range> Utopia::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
{
  std::vector<Range> ranges;
  return ranges;
}

/*
 * Utopia destructor cleans up each RestSeg, then destroys the buddy_allocator.
 */
Utopia::~Utopia()
{
  for (int i = 0; i < RestSegs; i++)
    delete RestSeg_vector[i];

  delete buddy_allocator;
}

/*
 * RestSeg destructor: deletes the internal RestSeg cache structure.
 */
RestSeg::~RestSeg()
{
  delete m_RestSeg_cache;
}
