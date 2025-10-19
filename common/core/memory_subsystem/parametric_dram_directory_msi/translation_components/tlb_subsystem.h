#pragma once
#ifndef TLB_SUBSYSTEM_H
#define TLB_SUBSYSTEM_H

#include "tlb.h"

namespace ParametricDramDirectoryMSI
{
	class MemoryManager;

	typedef std::vector<std::vector<TLB *>> TLBSubsystem;

	class TLBHierarchy
	{

	private:
		std::vector<std::vector<TLB *>> tlbLevels;
		std::vector<std::vector<TLB *>> data_path;
		std::vector<std::vector<TLB *>> instruction_path;
		int numLevels;
		int page_sizes;
		int numTLBsPerLevel;

		std::vector<std::vector<ComponentLatency>> tlb_latencies;

	public:
		TLBHierarchy(String mmu_name, Core *core, MemoryManager *memory_manager, ShmemPerfModel *shmem_perf_model);
		~TLBHierarchy();
		TLBSubsystem getTLBSubsystem() { return tlbLevels; }
		TLBSubsystem getDataPath() { return data_path; }
		TLBSubsystem getInstructionPath() { return instruction_path; }
		int getNumLevels() { return numLevels; }
	};

}

#endif