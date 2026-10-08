#include "simulator.h"
#include "cache.h"
#include "log.h"
#include "stats.h"
#include "config.hpp"
#include "debug_config.h"
#include <memory>
#include <fstream>
#include <tuple>
#include <iomanip>

// L2 set index logging
#if DEBUG_L2_SET_INDEX >= DEBUG_BASIC
static std::ofstream l2_set_index_log;
static bool l2_log_initialized = false;
#endif

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
	  sum_data_reuse(0),
	  sum_data_utilization(0),
	  sum_metadata_reuse(0),
	  sum_metadata_utilization(0),
	  number_of_data_reuse(0),
	  number_of_metadata_reuse(0),
	  metadata_passthrough_loc(Sim()->getCfg()->getInt("perf_model/metadata/passthrough_loc")),
	  m_core_id(core_id),
	  m_last_log_access_count(0),
	  m_content_log_initialized(false),
	  m_total_accesses(0)
{

	// Use of unique_ptr to avoid memory leaks
	m_pagesizes = std::unique_ptr<int[]>(new int[m_number_of_page_sizes]);
	for (int i = 0; i < m_number_of_page_sizes; i++)
	{
		m_pagesizes[i] = page_size[i];
	}

	for (int i = 0; i < 8; i++)
		data_util[i] = 0;
	for (int i = 0; i < 8; i++)
		metadata_util[i] = 0;


	for (int i = 0; i < 5; i++)
		data_reuse[i] = 0;
	for (int i = 0; i < 5; i++)
		metadata_reuse[i] = 0;



	reuse_levels[0] = 5; // kanellok Fix: these thresholds should be configurable
	reuse_levels[1] = 10;
	reuse_levels[2] = 20;

	m_set_info = CacheSet::createCacheSetInfo(name, cfgname, core_id, replacement_policy, m_associativity);
	m_sets = new CacheSet *[m_num_sets];
	m_fake_sets = nullptr; // Only allocate for L2 with metadata passthrough
	
	if(!is_tlb)
		std::cout << "[Memory Hierarchy] Creating " << name << " cache with " << m_num_sets << " sets, " << associativity << "-way associative, " << m_blocksize << "B blocksize" << std::endl;

	
	for (UInt32 i = 0; i < m_num_sets; i++)
	{
		m_sets[i] = CacheSet::createCacheSet(cfgname, core_id, replacement_policy, m_cache_type, m_associativity, m_blocksize, m_set_info);
	}

	if (name == "L2" && metadata_passthrough_loc > 2)
	{
		std::cout << "Creating one fake set for L2 cache since metadata cant be cached in L2" << std::endl;
		m_fake_sets = new CacheSet *[1]; // kanellok Hack: create a fake set for L2 cache to store metadata and "emulate" metadata bypassing
		m_fake_sets[0] = CacheSet::createCacheSet(cfgname, core_id, replacement_policy, m_cache_type, 1, m_blocksize, m_set_info);
	}

#ifdef ENABLE_SET_USAGE_HIST
	m_set_usage_hist = new UInt64[m_num_sets];
	for (UInt32 i = 0; i < m_num_sets; i++)
		m_set_usage_hist[i] = 0;
#endif

	m_page_walk_cacheblocks.clear();
	m_is_tlb = is_tlb;

	for (int i = 0; i < 5; i++)
	{

		registerStatsMetric(name, core_id, String("data-reuse-") + std::to_string(i).c_str(), &data_reuse[i]);
	}
	for (int i = 0; i < 5; i++)
	{

		registerStatsMetric(name, core_id, String("metadata-reuse-") + std::to_string(i).c_str(), &metadata_reuse[i]);
	}



	for (int i = 0; i < 8; i++)
	{

		registerStatsMetric(name, core_id, String("data-util-") + std::to_string(i).c_str(), &data_util[i]);
	}

	for (int i = 0; i < 8; i++)
	{

		registerStatsMetric(name, core_id, String("metadata-util-") + std::to_string(i).c_str(), &metadata_util[i]);
	}


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
	if (m_fake_sets != nullptr)
	{
		delete m_fake_sets[0];
		delete[] m_fake_sets;
	}

#if DEBUG_L2_SET_INDEX >= DEBUG_BASIC
	// Close the L2 set index log file if this is the L2 cache
	if (m_name == "L2" && l2_log_initialized) {
		l2_set_index_log.close();
		l2_log_initialized = false;
	}
#endif

#if DEBUG_L2_CONTENT >= DEBUG_BASIC
	// Close the L2 content log file
	if (m_name == "L2" && m_content_log_initialized && m_content_log.is_open()) {
		m_content_log.close();
	}
#endif
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

bool Cache::invalidateSingleLineTLB(IntPtr addr, int page_size)
{
	// Must iterate over ALL page sizes like accessSingleLineTLB does.
	// The entry could have been inserted with any page size, and the
	// set_index/tag differ based on page size.
	bool result = false;

	for (int ps = 0; ps < m_number_of_page_sizes; ps++)
	{
		IntPtr tag;
		UInt32 set_index;
		int current_page_size = m_pagesizes[ps];
		
		splitAddressTLB(addr, tag, set_index, current_page_size);
		assert(set_index < m_num_sets);
		
		// Use TLB-aware invalidate that matches BOTH tag AND page_size
		// This prevents invalidating entries from different page sizes that
		// happen to have the same tag (which maps to different VPNs)
		if (m_sets[set_index]->invalidateTLB(tag, current_page_size))
		{
			
			result = true;
			// Don't break - there might be entries at multiple page sizes
			// (though unlikely, being safe)
		}

	}
	//Assert if the entry is still present after invalidation attempts
	assert(!containsTLB(addr, page_size) && "Entry still present in TLB after invalidateSingleLineTLB");
	return result;
}

bool Cache::containsTLB(IntPtr addr, int page_size) const
{
	// Check if an entry exists for this address at any page size
	// Similar to invalidateSingleLineTLB but doesn't modify anything
	for (int ps = 0; ps < m_number_of_page_sizes; ps++)
	{
		IntPtr tag;
		UInt32 set_index;
		int current_page_size = m_pagesizes[ps];
		
		splitAddressTLB(addr, tag, set_index, current_page_size);
		assert(set_index < m_num_sets);
		
		// Use TLB-aware find that matches BOTH tag AND page_size
		if (m_sets[set_index]->findTLB(tag, current_page_size) != nullptr)
		{
			return true;
		}
	}
	
	return false;
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

	if (tlb_entry && !(cache_block_info->isPageTableBlock()))
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

#if DEBUG_L2_CONTENT >= DEBUG_BASIC
	// Periodically log L2 cache content distribution
	if (m_name == "L2") {
		m_total_accesses++;
		logCacheContentDistribution(m_total_accesses);
	}
#endif

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

	// Check if this is any type of metadata request using the helper function
	bool metadata_request = CacheBlockInfo::isMetadataBlockType(btype);

	if (m_name == "L2" && metadata_request && metadata_passthrough_loc > 2)
	{

		m_fake_sets[0]->insert(cache_block_info, fill_buff,
							   eviction, evict_block_info, evict_buff, cntlr);
	}
	else
	{
		m_sets[set_index]->insert(cache_block_info, fill_buff,
								  eviction, evict_block_info, evict_buff, cntlr);
	}

	*evict_addr = tagToAddress(evict_block_info->getTag());


	if ((*eviction) == true)
	{

		int reuse_value = evict_block_info->getReuse();

		// Simplified block types: DATA for regular data, PAGE_TABLE for all metadata types
		if (evict_block_info->getBlockType() == CacheBlockInfo::block_type_t::DATA ||
			evict_block_info->getBlockType() == CacheBlockInfo::block_type_t::INSTRUCTION)
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
			int set_bits = bits_set(evict_block_info->getUsage()) > 7 ? 7 : bits_set(evict_block_info->getUsage());
			data_util[set_bits]++;

			number_of_data_reuse++;
			average_data_reuse = sum_data_reuse / number_of_data_reuse;
		}
		// Check for metadata block types using helper function
		else if (CacheBlockInfo::isMetadataBlockType(evict_block_info->getBlockType()))
		{
			// Debug: Log when L1-D increments metadata stats

			
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
			int set_bits = bits_set(evict_block_info->getUsage()) > 7 ? 7 : bits_set(evict_block_info->getUsage());
			metadata_util[set_bits]++;

			number_of_metadata_reuse++;
			average_metadata_reuse = sum_metadata_reuse / number_of_metadata_reuse;
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




	for (int page_size = 0; page_size < m_number_of_page_sizes; page_size++)
	{ //  iterate over all possible page sizes
		int current_page_size = m_pagesizes[page_size];

		splitAddressTLB(addr, tag, set_index, block_offset, current_page_size); // provide the page size to find the index
		
		set = m_sets[set_index];
		// Use TLB-aware find that matches BOTH tag AND page_size
		// This prevents false matches between entries from different page sizes
		// (e.g., a 4KB and 2MB page that happen to have the same tag)
		cache_block_info = set->findTLB(tag, current_page_size, &line_index);


		if (cache_block_info == NULL)
			continue;

		// Check if the entry is valid (not invalidated)
		if (cache_block_info->getCState() == CacheState::INVALID)
		{
			continue;
		}


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
	splitAddressTLB(addr, tag, set_index, page_size);

	CacheBlockInfo *cache_block_info = CacheBlockInfo::create(m_cache_type);
	cache_block_info->setTag(tag);
	cache_block_info->setBlockType(btype);
	cache_block_info->setPageSize(page_size);
	cache_block_info->setPPN(ppn);
	cache_block_info->setCState(CacheState::SHARED);  // Mark as valid for TLB lookups
	m_sets[set_index]->insert(cache_block_info, fill_buff,
							  eviction, evict_block_info, evict_buff, cntlr);
	if (*eviction == true)
		page_size = evict_block_info->getPageSize();
	*evict_addr = tagToAddressTLB(evict_block_info->getTag(), page_size);
 


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

CacheSnapshot Cache::getCacheSnapshot() const
{
	CacheSnapshot snapshot;
	snapshot.num_sets = m_num_sets;
	snapshot.num_ways = m_associativity;
	snapshot.blocks.resize(m_num_sets);
	
	for (UInt32 set_idx = 0; set_idx < m_num_sets; ++set_idx)
	{
		snapshot.blocks[set_idx].resize(m_associativity);
		CacheSet* cache_set = m_sets[set_idx];
		
		for (UInt32 way = 0; way < m_associativity; ++way)
		{
			CacheBlockInfo* block_info = cache_set->peekBlock(way);
			CacheBlockSnapshot& block_snap = snapshot.blocks[set_idx][way];
			
			block_snap.valid = block_info->isValid();
			block_snap.block_type = static_cast<UInt8>(block_info->getBlockType());
			block_snap.recency = cache_set->getRecencyBits(way);
			block_snap.reuse_count = block_info->getReuse();
		}
	}
	
	return snapshot;
}

void Cache::saveSnapshotHeatmap(const std::string& filename) const
{
	// Get the snapshot
	CacheSnapshot snapshot = getCacheSnapshot();
	
	// Create a PPM image file (simple format, no external libs needed)
	// Image: rows = sets, cols = ways
	// Color encodes: block_type (hue) + recency (brightness)
	
	std::ofstream file(filename);
	if (!file.is_open())
	{
		LOG_PRINT_WARNING("Could not open file for cache heatmap: %s", filename.c_str());
		return;
	}
	
	// PPM header
	file << "P3\n";
	file << snapshot.num_ways << " " << snapshot.num_sets << "\n";
	file << "255\n";
	
	// Color palette for block types (R, G, B)
	// Invalid blocks are dark gray
	// Each block type gets a distinct color, with brightness based on recency
	auto getColor = [&](const CacheBlockSnapshot& block) -> std::tuple<int, int, int> {
		if (!block.valid) {
			return {40, 40, 40};  // Dark gray for invalid
		}
		
		// Base colors for different block types
		int r = 0, g = 0, b = 0;
		switch (static_cast<CacheBlockInfo::block_type_t>(block.block_type)) {
			case CacheBlockInfo::PAGE_TABLE_DATA:
				r = 255; g = 0; b = 0;   // Red (page table data)
				break;
			case CacheBlockInfo::PAGE_TABLE_INSTRUCTION:
				r = 255; g = 128; b = 0;   // Orange (page table instruction)
				break;
			case CacheBlockInfo::INSTRUCTION:
				r = 0; g = 255; b = 0;   // Green (instructions)
				break;
			case CacheBlockInfo::DATA:
			default:
				r = 0; g = 128; b = 255;  // Blue (normal data)
				break;
		}
		
		// Adjust brightness based on recency (0=MRU is brightest, higher=dimmer)
		// Normalize recency to 0-1 range based on associativity
		float recency_factor = 1.0f - (float)block.recency / (float)(snapshot.num_ways);
		recency_factor = 0.3f + 0.7f * recency_factor;  // Keep minimum brightness at 30%
		
		r = static_cast<int>(r * recency_factor);
		g = static_cast<int>(g * recency_factor);
		b = static_cast<int>(b * recency_factor);
		
		return {r, g, b};
	};
	
	// Write pixels
	for (UInt32 set_idx = 0; set_idx < snapshot.num_sets; ++set_idx)
	{
		for (UInt32 way = 0; way < snapshot.num_ways; ++way)
		{
			auto [r, g, b] = getColor(snapshot.blocks[set_idx][way]);
			file << r << " " << g << " " << b << " ";
		}
		file << "\n";
	}
	
	file.close();
}

void
Cache::logCacheContentDistribution(UInt64 access_count)
{
#if DEBUG_L2_CONTENT >= DEBUG_BASIC
	// Only log at specified intervals
	if (access_count < m_last_log_access_count + L2_CONTENT_LOG_INTERVAL) {
		return;
	}
	m_last_log_access_count = access_count;

	// Initialize log file on first call
	if (!m_content_log_initialized) {
		std::string log_filename = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/l2_content_core" + std::to_string(m_core_id) + ".csv";
		m_content_log.open(log_filename.c_str());
		if (m_content_log.is_open()) {
			m_content_log << "l2_accesses,total_blocks,valid_blocks,data_blocks,instruction_blocks,"
			              << "pt_data_blocks,pt_instr_blocks,"
			              << "invalid_blocks,data_pct,metadata_pct" << std::endl;
			m_content_log_initialized = true;
			std::cout << "[L2 Content] Opened log file: " << log_filename << std::endl;
		} else {
			std::cerr << "[L2 Content] Failed to open log file: " << log_filename << std::endl;
			return;
		}
	}

	// Count blocks by type (fine-grained)
	UInt64 total_blocks = 0;
	UInt64 valid_blocks = 0;
	UInt64 data_blocks = 0;
	UInt64 instruction_blocks = 0;
	UInt64 pt_data_blocks = 0;      // Page table for data translations
	UInt64 pt_instr_blocks = 0;     // Page table for instruction translations
	UInt64 utopia_blocks = 0;       // Utopia RestSeg metadata (FP, TAR, radix way table)
	UInt64 invalid_blocks = 0;

	for (UInt32 set_idx = 0; set_idx < m_num_sets; ++set_idx) {
		CacheSet* cache_set = m_sets[set_idx];
		for (UInt32 way = 0; way < m_associativity; ++way) {
			CacheBlockInfo* block_info = cache_set->peekBlock(way);
			total_blocks++;

			if (block_info->isValid()) {
				valid_blocks++;
				CacheBlockInfo::block_type_t block_type = block_info->getBlockType();
				switch (block_type) {
					case CacheBlockInfo::block_type_t::DATA:
						data_blocks++;
						break;
					case CacheBlockInfo::block_type_t::INSTRUCTION:
						instruction_blocks++;
						break;
					case CacheBlockInfo::block_type_t::PAGE_TABLE_DATA:
						pt_data_blocks++;
						break;
					case CacheBlockInfo::block_type_t::PAGE_TABLE_INSTRUCTION:
						pt_instr_blocks++;
						break;
					case CacheBlockInfo::block_type_t::UTOPIA_FP:
					case CacheBlockInfo::block_type_t::UTOPIA_TAR:
					case CacheBlockInfo::block_type_t::UTOPIA_RADIX_INTERNAL:
					case CacheBlockInfo::block_type_t::UTOPIA_RADIX_LEAF:
						utopia_blocks++;   // Utopia RestSeg metadata: translation metadata, not data
						break;
					default:
						// Treat unknown types as data
						data_blocks++;
						break;
				}
			} else {
				invalid_blocks++;
			}
		}
	}

	// Calculate percentages (of valid blocks)
	UInt64 total_metadata = pt_data_blocks + pt_instr_blocks + utopia_blocks;
	double data_pct = (valid_blocks > 0) ? (100.0 * (data_blocks + instruction_blocks) / valid_blocks) : 0.0;
	double metadata_pct = (valid_blocks > 0) ? (100.0 * total_metadata / valid_blocks) : 0.0;

	// Write to log
	m_content_log << access_count << ","
	              << total_blocks << ","
	              << valid_blocks << ","
	              << data_blocks << ","
	              << instruction_blocks << ","
	              << pt_data_blocks << ","
	              << pt_instr_blocks << ","
	              << invalid_blocks << ","
	              << std::fixed << std::setprecision(2) << data_pct << ","
	              << std::fixed << std::setprecision(2) << metadata_pct << std::endl;

#if DEBUG_L2_CONTENT >= DEBUG_DETAILED
	std::cout << "[L2 Content] Core " << m_core_id << " @ " << access_count << " accesses: "
	          << valid_blocks << "/" << total_blocks << " valid, "
	          << data_blocks << " data, " << instruction_blocks << " instr, "
	          << "PT[D:" << pt_data_blocks << " I:" << pt_instr_blocks << "] ("
	          << std::fixed << std::setprecision(1) << metadata_pct << "% metadata)" << std::endl;
#endif

#endif // DEBUG_L2_CONTENT
}
