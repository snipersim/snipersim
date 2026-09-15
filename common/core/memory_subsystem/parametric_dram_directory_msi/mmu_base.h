#pragma once
#include "cache_cntlr.h"
#include "memory_manager_base.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "stats.h"
#include "sim_log.h"
#include "metadata_info.h"

#define MMU_MAX_NUMA_NODES 8

namespace ParametricDramDirectoryMSI
{
    /**
     * @brief Result of a complete page table walk operation.
     * 
     * This struct encapsulates the outcome of the entire PTW process
     * including the translation, timing, and whether a fault occurred.
     */
    struct PTWOutcome
    {
        SubsecondTime latency;  ///< Total latency of the page table walk
        bool page_fault;        ///< True if a page fault occurred
        IntPtr ppn;             ///< Physical page number (translation result)
        int page_size;          ///< Page size in bits (12=4KB, 21=2MB, 30=1GB)
        int requested_frames;   ///< Number of frames needed for page fault (PT frames + data frame)
        uint64_t payload_bits;  ///< Shadow PTE payload (temporal offset entries for prefetching)

        PTWOutcome() : latency(SubsecondTime::Zero()), page_fault(false), ppn(0), page_size(0), requested_frames(0), payload_bits(0) {}

        PTWOutcome(SubsecondTime latency, bool page_fault, IntPtr ppn, int page_size, int requested_frames = 0, uint64_t payload_bits = 0)
            : latency(latency), page_fault(page_fault), ppn(ppn), page_size(page_size), requested_frames(requested_frames), payload_bits(payload_bits) {}
    };

    class TLBHierarchy;
    class BaseFilter;

    class MemoryManagementUnitBase
    {

    protected:

        Core *core;
        MemoryManagerBase *memory_manager;
        ShmemPerfModel *shmem_perf_model;
        String name;
        MemoryManagementUnitBase *nested_mmu;

        accessedAddresses accesses_for_nest;
        
        bool is_guest;
        int dram_accesses_during_last_walk;
        
        // PTW ID counter for tracking individual page table walks
        UInt64 m_ptw_id_counter;
        
        
        std::ofstream ptw_dump_file;
        std::string ptw_dump_filename;
        UInt64 tlb_hits_since_last_ptw;  ///< Count of TLB hits between consecutive PTWs (for temporal analysis)
        
        // Per-access PTW logging (runtime config)
        bool ptw_access_logging_enabled;
        int ptw_access_logging_sample_rate;
        UInt64 ptw_access_log_counter;
        std::ofstream ptw_access_log_file;

        // SimLog for structured logging
        SimLog *mmu_base_log;

        struct {
            UInt64 DRAM_accesses;           // Total DRAM accesses (local + remote)
            UInt64 DRAM_accesses_local;      // DRAM accesses that hit local NUMA node
            UInt64 DRAM_accesses_remote;     // DRAM accesses that hit remote NUMA node
            UInt64 DRAM_accesses_per_numa_node[MMU_MAX_NUMA_NODES]; // Per-NUMA-node breakdown
            UInt64 L1D_accesses;
            UInt64 L2_accesses;
            UInt64 NUCA_accesses;
            // Prefetch-only walker stats (subset of the above totals)
            UInt64 DRAM_accesses_prefetch;
            UInt64 L1D_accesses_prefetch;
            UInt64 L2_accesses_prefetch;
            UInt64 NUCA_accesses_prefetch;
        } walker_stats;

        UInt32 m_num_numa_nodes;
        bool m_numa_enabled;

        bool count_page_fault_latency_enabled;
        bool perfect_translation_enabled;  // If true: translation happens (PA remapping) but with zero latency



        struct{
            UInt64 memory_accesses;
            UInt64 memory_accesses_before_filtering;
        } stats;

    public:
        struct translationPacket
        {
            IntPtr address;
            IntPtr eip;
            bool instruction;
            Core::lock_signal_t lock_signal;
            CacheBlockInfo::block_type_t type;
            bool modeled;
            bool count;
            translationPacket(IntPtr _address, IntPtr _eip, bool _instruction, Core::lock_signal_t _lock_signal, bool _modeled, bool _count, CacheBlockInfo::block_type_t _type) : address(_address),
                                                                                                                                                                                   eip(_eip),
                                                                                                                                                                                   instruction(_instruction),
                                                                                                                                                                                   lock_signal(_lock_signal),
                                                                                                                                                                                   type(_type),
                                                                                                                                                                                   modeled(_modeled),
                                                                                                                                                                                   count(_count)
            {
            }
            translationPacket()
            {
            }
        };

    public:
		MemoryManagementUnitBase(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase* _nested_mmu);
        virtual ~MemoryManagementUnitBase() = 0;

		void instantiatePageTableWalker();
		void instantiateTLBSubsystem();
		virtual void registerMMUStats() = 0;
		accessedAddresses getAccessesForNest()
        {
			return accesses_for_nest;
        }

		void clearAccessesForNest(){
			accesses_for_nest.clear();
		}

		virtual IntPtr performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count) = 0; //Returns translation latency + translated address (physical address)
        virtual SubsecondTime accessCache(translationPacket packet, SubsecondTime t_start, bool is_prefetch, HitWhere::where_t& out_hit_where);		
        virtual PTWResult filterPTWResult(IntPtr address, PTWResult ptw_result, PageTable *page_table, bool count) = 0;
		virtual BaseFilter* getPTWFilter() { return nullptr; }
		virtual void discoverVMAs() = 0;
        
		// Called when a TLB level evicts an entry to make room for a new one.
		// Default no-op; Victima overrides it to fetch metadata for the evicted
		// VPN in the background.
		virtual void onTLBLevelEviction(int /*level*/, IntPtr /*evicted_address*/,
		                                int /*evicted_page_size*/, IntPtr /*evicted_ppn*/,
		                                IntPtr /*eip*/, Core::lock_signal_t /*lock*/,
		                                bool /*instruction*/) {}

		virtual PTWOutcome performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch, IntPtr eip, Core::lock_signal_t lock, PageTable *page_table, bool restart_walk, bool instruction = false);
        
        /**
         * @brief Perform translation without timing overhead (for perfect_translation mode)
         * 
         * This function queries the page table for the translation, handling page faults
         * if necessary, but does NOT charge any TLB or PTW latencies. It's used to isolate
         * the cache behavior benefit of PA remapping from the translation overhead.
         *
         * @param address The virtual address to translate
         * @param page_table Pointer to the page table
         * @return std::pair<IntPtr, int> containing (physical_address, page_size_bits)
         */
        std::pair<IntPtr, int> translateWithoutTiming(IntPtr address, PageTable *page_table);
        
        pair<SubsecondTime, SubsecondTime> calculatePFCycles(PTWResult ptw_result, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock);
        SubsecondTime calculatePTWCycles(PTWResult ptw_result, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock, IntPtr original_va, bool instruction = false, bool is_prefetch = false);		
        Core *getCore() { return core; }
		String getName() { return name; }
        int getDramAccessesDuringLastWalk() { return dram_accesses_during_last_walk; }
        UInt64 getLastPtwId() { return m_ptw_id_counter; }  // Get the last PTW ID for correlation

    protected:
        /**
         * @brief Virtual hook for logging individual PTW cache accesses
         * 
         * Called by calculatePTWCycles for each cache access during a page table walk.
         * Default implementation logs to ptw_access_log_file when enabled via config.
         * Override in derived classes for custom logging behavior.
         * 
         * @param ptw_id Unique PTW operation ID
         * @param vpn Virtual page number being translated
         * @param level Page table level (depth)
         * @param table Table type (0=4KB, 1=2MB, 2=1GB)
         * @param cache_line_addr Cache line address (physical_addr >> 6)
         * @param hit_where Where the access hit (L1/L2/NUCA/DRAM)
         * @param latency Access latency
         * @param is_pte Whether this is the final PTE
         */
        virtual void logPTWCacheAccess(UInt64 ptw_id, UInt64 vpn, int level, int table,
                                       IntPtr cache_line_addr, HitWhere::where_t hit_where,
                                       SubsecondTime latency, bool is_pte);

    };
}
