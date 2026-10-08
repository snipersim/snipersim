#ifndef __CACHE_BLOCK_INFO_H__
#define __CACHE_BLOCK_INFO_H__

#include "fixed_types.h"
#include "cache_state.h"
#include "cache_base.h"

class CacheBlockInfo
{
public:
	enum option_t
	{
		PREFETCH,
		WARMUP,
		// Appended rather than inserted after PREFETCH, where Virtuoso has it:
		// options are a bitmask and inserting would renumber WARMUP.
		PREFETCH_FROM_DRAM,   // set with PREFETCH when the fill came from DRAM rather than NUCA/LLC
		NUM_OPTIONS
	};

	enum block_type_t
	{
		PAGE_TABLE_DATA	= 0, // Page table data (e.g., PTEs for data pages)
		PAGE_TABLE_INSTRUCTION, // Page table data (e.g., PTEs for instruction pages)
		INSTRUCTION,         // Instruction fetches
		DATA,                // Regular data accesses
		VICTIMA_TLB_BLOCK,   // Victima: a leaf PTE line used as a TLB block (8 PTEs / 64B)
		// Utopia (Kanellopoulos et al., MICRO 2023) RestSeg metadata, appended
		// after VICTIMA_TLB_BLOCK so that no existing type is renumbered
		UTOPIA_FP,           // fingerprint array line
		UTOPIA_TAR,          // tag array line
		UTOPIA_RADIX_INTERNAL, // radix way table internal node (pointer array)
		UTOPIA_RADIX_LEAF,   // radix way table leaf (packed valid+way entries)
		NUM_BLOCK_TYPES
	};

	// Helper function to check if a block type is metadata (any translation-related block)
	static inline bool isMetadataBlockType(block_type_t block_type) {
		return (block_type == PAGE_TABLE_DATA ||
		        block_type == PAGE_TABLE_INSTRUCTION ||
		        block_type == VICTIMA_TLB_BLOCK ||
		        block_type == UTOPIA_FP ||
		        block_type == UTOPIA_TAR ||
		        block_type == UTOPIA_RADIX_INTERNAL ||
		        block_type == UTOPIA_RADIX_LEAF);
	}
	
	// For backward compatibility - PAGE_TABLE is an alias for PAGE_TABLE_DATA
	static const block_type_t PAGE_TABLE = PAGE_TABLE_DATA;

	static const UInt8 BitsUsedOffset = 3; // Track usage on 1<<BitsUsedOffset granularity (per 64-bit / 8-byte)
	typedef UInt8 BitsUsedType;			   // Enough to store one bit per 1<<BitsUsedOffset byte element per cache line (8 8-byte elements for 64-byte cache lines)
										   // This can be extended later to include other information
										   // for different cache coherence protocols
private:

	//  for TLB: added ppn to store the physical page number
	IntPtr ppn;
	bool m_tlb_entry;	  //  for caches that store TLB entries: flag to indicate if this is a TLB entry: this was used only in [Kanellopoulos et al. Victima MICRO 2023]
	int m_page_size;	  // for TLBs: hold page size for each cache block

	IntPtr m_tag;
	CacheState::cstate_t m_cstate;
	UInt64 m_owner;
	BitsUsedType m_used;
	// Per 8-byte sub-region: parts of an MMU-prefetched line that a demand
	// access has not yet been credited for.
	BitsUsedType m_prefetch_pending;
	UInt8 m_options; // large enough to hold a bitfield for all available option_t's
	block_type_t m_block_type;
	int m_reuse; // tracking reuse
	int utilization;

	static const char *option_names[];

public:
	CacheBlockInfo(IntPtr tag = ~0,
				   CacheState::cstate_t cstate = CacheState::INVALID,
				   UInt64 options = 0);

	virtual ~CacheBlockInfo();

	static CacheBlockInfo *create(CacheBase::cache_t cache_type);
	virtual void invalidate(void);
	virtual void clone(CacheBlockInfo *cache_block_info);

	bool isValid() const { return (m_tag != ((IntPtr)~0)); }


	IntPtr getTag() const { return m_tag; }

	CacheState::cstate_t getCState() const { return m_cstate; }

	void setTag(IntPtr tag) { m_tag = tag; }

	void setCState(CacheState::cstate_t cstate) { m_cstate = cstate; }
	void setPPN(IntPtr _ppn) { ppn = _ppn; };
	IntPtr getPPN() { return ppn; };
	UInt64 getOwner() const { return m_owner; }
	void setOwner(UInt64 owner) { m_owner = owner; }

	bool hasOption(option_t option) { return m_options & (1 << option); }
	void setOption(option_t option) { m_options |= (1 << option); }
	void clearOption(option_t option) { m_options &= ~(UInt64(1) << option); }

	inline void setBlockType(block_type_t bt) { m_block_type = bt; }
	inline block_type_t getBlockType() { return m_block_type; }
	inline bool isPageTableBlock() { return isMetadataBlockType(m_block_type); }
	inline bool isInstructionBlock() { return (m_block_type == block_type_t::INSTRUCTION); }
	inline bool isDataBlock() { return (m_block_type == block_type_t::DATA); }

	void setPageSize(int pagesize) { m_page_size = pagesize; }


	int getPageSize() { return m_page_size; }

	BitsUsedType getUsage() const { return m_used; };
	bool updateUsage(UInt32 offset, UInt32 size);

	// A prefetched 64-byte PTE line carries up to eight PTEs, so the single
	// PREFETCH bit can only be spent once and undercounts what the prefetch
	// supplied.  markPrefetchedLine() marks every 8-byte sub-region as awaiting
	// credit; consumePrefetch() credits the ones a demand access touches and
	// clears them, so each PTE is counted at most once.
	void markPrefetchedLine() { m_prefetch_pending = (BitsUsedType)~0; }
	bool consumePrefetch(UInt32 offset, UInt32 size);
	bool updateUsage(BitsUsedType used);

	static const char *getOptionName(option_t option);

	void increaseReuse() { m_reuse++; }
	int getReuse() { return m_reuse; }
};

class CacheCntlr
{
public:
	virtual bool isInLowerLevelCache(CacheBlockInfo *block_info) { return false; }
	virtual void incrementQBSLookupCost() {}
};

#endif /* __CACHE_BLOCK_INFO_H__ */
