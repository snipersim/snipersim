#pragma once
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "stats.h"

namespace ParametricDramDirectoryMSI
{

    class TLBHierarchy;

    class MemoryManagementUnitBase
    {

    protected:
        Core *core;
        MemoryManager *memory_manager;
        ShmemPerfModel *shmem_perf_model;
        String name;

        int dram_accesses_during_last_walk;

        std::ofstream log_file_mmu;
        std::string log_file_name_mmu;

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
		MemoryManagementUnitBase(Core *_core, MemoryManager *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name);
        
		void instantiatePageTableWalker();
		void instantiateTLBSubsystem();
		virtual void registerMMUStats() = 0;

		virtual IntPtr performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count) = 0; //Returns translation latency + translated address (physical address)
		virtual SubsecondTime accessCache(translationPacket packet, SubsecondTime t_start = SubsecondTime::Zero(),bool is_prefetch = false);
		virtual PTWResult filterPTWResult(PTWResult ptw_result, PageTable *page_table, bool count) = 0;
		virtual void discoverVMAs() = 0;
        
        int getDramAccessesDuringLastWalk() { return dram_accesses_during_last_walk; }
		virtual tuple<SubsecondTime, bool, IntPtr, int> performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch, IntPtr eip, Core::lock_signal_t lock, PageTable *page_table, bool restart_walk);
        pair<SubsecondTime, SubsecondTime> calculatePFCycles(PTWResult ptw_result, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock);
		SubsecondTime calculatePTWCycles(PTWResult ptw_result, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock);
		Core *getCore() { return core; }
		String getName() { return name; }
    };
}