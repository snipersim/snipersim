
#pragma once
#include "../memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "mmu_base.h"
#include "metadata_table_base.h"
#include "ptmshrs.h"
#include "base_filter.h"

namespace ParametricDramDirectoryMSI
{

	class TLBHierarchy;

    class HWFaultHandler
    {
    public:
        IntPtr start_ppn;
        IntPtr end_ppn;
        int number_of_pages;
        IntPtr *tag_array;
        int page_size;
        IntPtr tag_array_start_ppn;
        IntPtr tag_array_end_ppn;
        UInt64 hw_handled_faults;
        UInt64 fallback_faults;

        HWFaultHandler()
            : start_ppn(0), end_ppn(0), number_of_pages(0),
              tag_array(nullptr), page_size(12),
              tag_array_start_ppn(0), tag_array_end_ppn(0),
              hw_handled_faults(0), fallback_faults(0) {}

        HWFaultHandler(IntPtr start_address, IntPtr end_address, int _page_size)
            : tag_array(nullptr), page_size(_page_size),
              hw_handled_faults(0), fallback_faults(0)
        {
            start_ppn = start_address >> page_size;
            end_ppn = end_address >> page_size;
            number_of_pages = end_ppn - start_ppn + 1;
            tag_array = new IntPtr[number_of_pages];
            for (int i = 0; i < number_of_pages; i++)
                tag_array[i] = static_cast<IntPtr>(-1);
        }

        HWFaultHandler(HWFaultHandler &&o) noexcept
            : start_ppn(o.start_ppn), end_ppn(o.end_ppn),
              number_of_pages(o.number_of_pages), tag_array(o.tag_array),
              page_size(o.page_size), tag_array_start_ppn(o.tag_array_start_ppn),
              tag_array_end_ppn(o.tag_array_end_ppn),
              hw_handled_faults(o.hw_handled_faults), fallback_faults(o.fallback_faults)
        { o.tag_array = nullptr; o.number_of_pages = 0; }

        HWFaultHandler& operator=(HWFaultHandler &&o) noexcept {
            if (this != &o) {
                delete[] tag_array;
                start_ppn = o.start_ppn; end_ppn = o.end_ppn;
                number_of_pages = o.number_of_pages; tag_array = o.tag_array;
                page_size = o.page_size;
                tag_array_start_ppn = o.tag_array_start_ppn;
                tag_array_end_ppn = o.tag_array_end_ppn;
                hw_handled_faults = o.hw_handled_faults;
                fallback_faults = o.fallback_faults;
                o.tag_array = nullptr; o.number_of_pages = 0;
            }
            return *this;
        }
        HWFaultHandler(const HWFaultHandler&) = delete;
        HWFaultHandler& operator=(const HWFaultHandler&) = delete;

        ~HWFaultHandler() { delete[] tag_array; }

        bool canHandleFault(IntPtr address) {
            IntPtr ppn = (address >> page_size) % number_of_pages;
            return (tag_array[ppn] == static_cast<IntPtr>(-1));
        }

        void recordMapping(IntPtr address) {
            IntPtr ppn = (address >> page_size) % number_of_pages;
            tag_array[ppn] = address >> page_size;
        }
    };

	class MemoryManagementUnitHWFault : public MemoryManagementUnitBase
	{
	private:
		MemoryManagerBase *memory_manager;
		TLBHierarchy *tlb_subsystem;
		MetadataTableBase *metadata_table;
		MSHR *pt_walkers;
		BaseFilter *ptw_filter;
        HWFaultHandler hw_fault_handler;
		std::ofstream log_file;
		std::string log_file_name;

		SubsecondTime hw_fault_latency;

		struct
		{
			UInt64 num_translations;
			UInt64 page_faults;
			UInt64 page_faults_hw_handled;
			UInt64 page_faults_os_handled;
			UInt64 page_table_walks;
			SubsecondTime total_walk_latency;
			SubsecondTime total_translation_latency;
			SubsecondTime total_tlb_latency;
			SubsecondTime total_fault_latency;
			SubsecondTime total_hw_fault_latency;
			SubsecondTime walker_is_active;
			SubsecondTime *tlb_latency_per_level;
			UInt64 *tlb_hit_page_sizes;
		} translation_stats;

	public:
		MemoryManagementUnitHWFault(Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu);
		~MemoryManagementUnitHWFault();

		void instantiatePageTableWalker();
		void instantiateMetadataTable();
		void instantiateTLBSubsystem();
        void instantiateHWFaultHandler();
		void registerMMUStats();
		void discoverVMAs();

		BaseFilter *getPTWFilter() override { return ptw_filter; }

		PTWResult filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count);
		IntPtr performAddressTranslation(IntPtr eip, IntPtr virtual_address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);

	};

}
