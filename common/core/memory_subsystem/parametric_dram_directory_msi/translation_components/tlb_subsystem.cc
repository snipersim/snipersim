

#include "tlb_subsystem.h"
#include "tlb.h"
#include <boost/algorithm/string.hpp>
#include "config.hpp"
#include "stride_prefetcher.h"
#include "tlb_prefetcher_factory.h"
#include "pagesize_predictor_factory.h"
#include "dvfs_manager.h"

using namespace boost::algorithm;
using namespace std;

namespace ParametricDramDirectoryMSI
{

    TLBHierarchy::TLBHierarchy(String mmu_name, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
    {
        
            std::cout << "[MMU] Instantiating TLB Hierarchy" << std::endl;
            page_size_predictor = NULL;
            numLevels = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_subsystem/number_of_levels");
        
            prefetch_enabled = Sim()->getCfg()->getBool("perf_model/"+mmu_name+"/tlb_subsystem/prefetch_enabled");
        
            tlbLevels.resize(numLevels);
            data_path.resize(numLevels);
            instruction_path.resize(numLevels);
            tlb_latencies.resize(numLevels);

            std::cout << "[TLB] Number of Levels: " << numLevels << std::endl;

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
                    free(page_size_list);

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
            
                if (prefetch_enabled)
                {
                    int prefetcher_level = (Sim()->getCfg()->getInt("perf_model/"+ mmu_name + "/tlb_prefetch/level"));

                    if (prefetcher_level != level)
                        continue;

                    std::cout << "[TLB] Prefetch Enabled at Level: " << level << std::endl;
                    int numpqs = (Sim()->getCfg()->getInt("perf_model/"+ mmu_name + "/tlb_prefetch/number_of_pqs"));

                    tlbLevels[level-1].reserve(numpqs);
                    data_path[level-1].reserve(numpqs);

                    instruction_path[level-1].reserve(numpqs);
                    tlb_latencies[level-1].reserve(numpqs);

                    for (int pqIndex = 1; pqIndex <= numpqs; pqIndex++)
                    {
                        std::string pqIndex_str = std::to_string(pqIndex);
                        String pqIndexString = String(pqIndex_str.begin(), pqIndex_str.end());
                        String pqconfigstring = "perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString;
                        String type = Sim()->getCfg()->getString("perf_model/"+ mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/type");

                        int page_size_count = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/page_size");
                        int *page_size_list = (int *)malloc(sizeof(int) * page_size_count);

                        for (int i = 0; i < page_size_count; i++)
                        {
                            page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/page_size_list", i);
                        }

                        int size = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/size");
                        std::string pqName = "PQ_" + std::to_string(pqIndex);
                        String pqname = String(pqName.begin(), pqName.end());
                        int number_of_prefetchers = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/number_of_prefetchers");
                        TLBPrefetcherBase **prefetchers = (TLBPrefetcherBase **)malloc(sizeof(TLBPrefetcherBase *) * number_of_prefetchers);

                        std::cout << "[TLB] Number of Prefetchers for PQ " << pqIndex << ": " << number_of_prefetchers << std::endl;


                        for (int i = 0; i < number_of_prefetchers; i++)
                        {
                            String name = Sim()->getCfg()->getStringArray("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/prefetcher_list", i);
                            prefetchers[i] = TLBprefetcherFactory::createTLBPrefetcher(mmu_name, name, pqIndexString, core, memory_manager, shmem_perf_model);
                            std::cout << "[TLB] Created TLB Prefetcher of type: " << name << " for PQ " << pqIndex << std::endl;

                        }

                        ComponentLatency latency = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/access_latency"));
                        int assoc = Sim()->getCfg()->getInt("perf_model/" + mmu_name + "/tlb_prefetch/pq" + pqIndexString + "/assoc");
                        TLB *pq = new TLB(pqname, pqconfigstring, core->getId(), latency, size, assoc, page_size_list, page_size_count, type, false, true, prefetchers, number_of_prefetchers);

                        std::cout << "[TLB] Created TLB Prefetch Queue of size: " << size << " for PQ " << pqIndex << std::endl;
                        std::cout << "[TLB] PQ Properties - Size: " << size << ", Assoc: " << assoc << ", Page Size Count: " << page_size_count << std::endl;

                        tlbLevels[level-1].push_back(pq);
                        free(page_size_list);
                        if (type == "Data")
                            data_path[level-1].push_back(pq);
                        else if (type == "Instruction")
                            instruction_path[level-1].push_back(pq);
                        else
                        {
                            data_path[level-1].push_back(pq);
                            instruction_path[level-1].push_back(pq);
                        }
                    }
                }
            }

        String predictor_type = Sim()->getCfg()->getString("perf_model/" + mmu_name + "/tlb_subsystem/page_size_predictor/type");
        page_size_predictor = PagesizePredictorFactory::createPagesizePredictor(predictor_type, core);

        // After all TLBs are built, give prefetchers access to the full TLB hierarchy
        // for residency checks (e.g., skip prefetching regions already in TLB)
        if (prefetch_enabled)
        {
            // Flatten all TLBs across all levels into a single vector
            std::vector<TLB*> all_tlbs;
            for (auto &level : tlbLevels)
                for (auto *tlb : level)
                    all_tlbs.push_back(tlb);

            // Set TLB hierarchy on all prefetchers attached to PQ TLBs
            for (auto &level : tlbLevels)
            {
                for (auto *tlb : level)
                {
                    if (tlb->getPrefetch() && tlb->getPrefetchers() != nullptr)
                    {
                        for (int i = 0; i < tlb->getNumPrefetchers(); i++)
                            tlb->getPrefetchers()[i]->setTLBHierarchy(all_tlbs);
                    }
                }
            }

            // Register PQ prefetchers as victim observers on the non-PQ TLBs
            // at the same level.  When those TLBs evict entries, the eviction
            // is forwarded to the PQ prefetchers
            // via notifyVictim().  Only same-level wiring is needed: L1
            // evictions cascade into L2 allocations (possibly causing L2
            // evictions that ARE forwarded), so an L1 eviction alone does NOT
            // mean the translation left the TLB hierarchy.
            for (auto &level : tlbLevels)
            {
                std::vector<TLBPrefetcherBase*> pq_prefetchers;
                for (auto *tlb : level)
                {
                    if (tlb->getPrefetch() && tlb->getPrefetchers() != nullptr)
                    {
                        for (int i = 0; i < tlb->getNumPrefetchers(); i++)
                            pq_prefetchers.push_back(tlb->getPrefetchers()[i]);
                    }
                }
                if (!pq_prefetchers.empty())
                {
                    for (auto *tlb : level)
                    {
                        if (!tlb->getPrefetch())
                        {
                            for (auto *pref : pq_prefetchers)
                                tlb->addVictimObserver(pref);
                        }
                    }
                }
            }
        }

    }

    /*
    This function predicts the page size based on the page size predictor.
     * If the page size predictor is not set, it returns 0 if no page sizes are configured,
     * otherwise it returns 12 (indicating a default page size of 4KB).
     *
     * @return The predicted page size in bits, or 0 if no page sizes are configured.

     Make use of these functions inside the MMU to predict the page size and update the predictor after a memory access.
    */
    int TLBHierarchy::predictPagesize(IntPtr virtual_address)
    {
        if (page_size_predictor == NULL)
        {

            return 12;
        }
        return page_size_predictor->predictPageSize(virtual_address);
    }

    void TLBHierarchy::updatePageSizePredictor(IntPtr virtual_address, int page_size)
    {
        if (page_size_predictor == NULL)
        {
            return;
        }
        return page_size_predictor->update(virtual_address, page_size);
    }

    TLBHierarchy::~TLBHierarchy()
    {

        for (auto &level : tlbLevels)
        {
            for (auto *tlb : level)
            {
                delete tlb;
            }
            level.clear();
        }
        tlbLevels.clear();
        data_path.clear();
        instruction_path.clear();
        tlb_latencies.clear();
        delete page_size_predictor;
        page_size_predictor = NULL;
    }

}
