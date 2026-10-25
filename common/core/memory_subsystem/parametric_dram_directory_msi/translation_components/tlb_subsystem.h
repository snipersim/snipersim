#pragma once
#ifndef TLB_SUBSYSTEM_H
#define TLB_SUBSYSTEM_H

#include "tlb.h"
#include "tlb_prefetcher_base.h"
#include "pagesize_predictor_base.h"

namespace ParametricDramDirectoryMSI
{

	typedef std::vector<std::vector<TLB *>> TLBSubsystem;

	class TLBHierarchy
	{

	private:
		std::vector<std::vector<TLB *>> tlbLevels;
		std::vector<std::vector<TLB *>> data_path;
		std::vector<std::vector<TLB *>> instruction_path;
		PageSizePredictorBase *page_size_predictor;
		int numLevels;
		// int *page_size_list;
		int page_sizes;
		int numTLBsPerLevel;

		std::vector<std::vector<ComponentLatency>> tlb_latencies;

	public:
		bool prefetch_enabled;
		TLBHierarchy(String mmu_name, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model);
		~TLBHierarchy();
		TLBSubsystem getTLBSubsystem() { return tlbLevels; }
		TLBSubsystem getDataPath() { return data_path; }
		TLBSubsystem getInstructionPath() { return instruction_path; }
		bool isPrefetchEnabled() { return prefetch_enabled; }
		int getNumLevels() { return numLevels; }
		int predictPagesize(IntPtr eip);
		void updatePageSizePredictor(IntPtr virtual_address, int page_size);
		void predictionResult(IntPtr eip, bool success);
	};

}

#endif
