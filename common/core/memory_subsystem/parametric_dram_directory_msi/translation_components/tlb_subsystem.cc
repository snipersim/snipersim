

#include "tlb_subsystem.h"
#include "tlb.h"
#include "../memory_manager.h"
#include <boost/algorithm/string.hpp>
#include "config.hpp"
#include "dvfs_manager.h"

using namespace boost::algorithm;
using namespace std;

namespace ParametricDramDirectoryMSI
{

	TLBHierarchy::TLBHierarchy(String mmu_name, Core *core, MemoryManager *memory_manager, ShmemPerfModel *shmem_perf_model)
	{
		
            std::cout << "[MMU] Instantiating TLB Hierarchy" << std::endl;
            numLevels = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_subsystem/number_of_levels");
            
            int add_extra_level = 0;
        
            tlbLevels.resize(numLevels + add_extra_level);
            data_path.resize(numLevels + add_extra_level);
            instruction_path.resize(numLevels + add_extra_level);
            tlb_latencies.resize(numLevels + add_extra_level);

        for (int level = 1; level <= numLevels; ++level)
        {
            std::string level_str = std::to_string(level);
            String levelString = String(level_str.begin(), level_str.end());

            int numTLBs = (Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/number_of_tlbs"));

            tlbLevels[level - 1].reserve(numTLBs);
            data_path[level - 1].reserve(numTLBs);
            instruction_path[level - 1].reserve(numTLBs);
            tlb_latencies[level - 1].reserve(numTLBs);

            for (int tlbIndex = 1; tlbIndex <= numTLBs; ++tlbIndex)
            {

                std::string tlbIndex_str = std::to_string(tlbIndex);

                String tlbIndexString = String(tlbIndex_str.begin(), tlbIndex_str.end());
                String tlbconfigstring = "perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString;
                String type = Sim()->getCfg()->getString("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/type");
                int size = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/size");
                int assoc = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/assoc");
                page_sizes = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/page_size");
                int *page_size_list = (int *)malloc(sizeof(int) * (page_sizes));
                bool allocate_on_miss = Sim()->getCfg()->getBool("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/allocate_on_miss");

                ComponentLatency latency = ComponentLatency(core ? core->getDvfsDomain() : Sim()->getDvfsManager()->getGlobalDomain(DvfsManager::DvfsGlobalDomain::DOMAIN_GLOBAL_DEFAULT), Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/access_latency"));

                for (int i = 0; i < page_sizes; i++)
                    page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mmu_name + "/tlb_level_" + levelString + "/tlb" + tlbIndexString + "/page_size_list", i);

                std::string tlbName = "TLB_L" + std::to_string(level) + "_" + std::to_string(tlbIndex);
                String tlbname = String(tlbName.begin(), tlbName.end());
                String full_name = mmu_name + "_" + tlbname;

                TLB *tlb = new TLB(full_name, tlbconfigstring, core ? core->getId() : 0, latency, size, assoc, page_size_list, page_sizes, type, allocate_on_miss);

                tlbLevels[level - 1].push_back(tlb);

                if (type == "Data")
                    data_path[level - 1].push_back(tlb);
                else if (type == "Instruction")
                    instruction_path[level - 1].push_back(tlb);
                else
                {
                    data_path[level - 1].push_back(tlb);
                    instruction_path[level - 1].push_back(tlb);
                }
            }
        

        }


    }



    TLBHierarchy::~TLBHierarchy()
    {

    }

}
