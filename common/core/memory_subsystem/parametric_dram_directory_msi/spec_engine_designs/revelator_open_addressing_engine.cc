#include "revelator_open_addressing_engine.h" // Updated include
#include "simulator.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "cache_block_info.h"
#include "stats.h"
#include "mimicos.h"
#include "dvfs_manager.h"
#include "dram_cntlr_interface.h"

//#define DEBUG_REVELATOR

namespace ParametricDramDirectoryMSI
{
    RevelatorOpenAddressingEngine::RevelatorOpenAddressingEngine(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name) : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name), name(_name), memory_manager(_memory_manager)
    {
#ifdef DEBUG_REVELATOR
        std::cout << "[RevelatorOpenAddressingEngine] Initializing Engine" << std::endl;
#endif
        // Getting config values
        m_probing_depth = Sim()->getCfg()->getInt("perf_model/revelator_open/probing_depth");      // Probing depth for open addressing
        m_memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/revelator_open/memory_size");      // total memory size
        kernel_size = Sim()->getCfg()->getInt("perf_model/revelator_open/kernel_size");          // total kernel size
        oracle_revelator = Sim()->getCfg()->getBool("perf_model/revelator_open/oracle");          // whether we are doing perfect refresh
        rev_type = Sim()->getCfg()->getInt("perf_model/revelator_open/type");              // rev type 0=full 1=data 2=translation only
        has_filter = Sim()->getCfg()->getBool("perf_model/revelator_open/filter");
        perfect_filtering = Sim()->getCfg()->getBool("perf_model/revelator_open/perfect_filtering"); // whether we are doing perfect refresh
        
        m_total_pages = m_memory_size * 1024 / 4 - kernel_size * 1024 / 4; // calculating total pages
        m_kernel_pages = kernel_size * 1024 / 4;                          // calculating kernel pages

        if (Sim()->getMimicOS()->getMemoryAllocator()->getName() != "revelator_open_addressing")
        {
            std::cout << "[RevelatorOpenAddressingEngine] WARNING: Memory allocator may not be compatible." << std::endl;
            // exit(1); // This check might be too strict, changed to a warning.
        }

        log_file_name = std::string(name.c_str()) + ".revelator_open.log";
        log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
        log_file.open(log_file_name);

        // initializing stats
        hits_per_probe = new UInt64[m_probing_depth];
        prefetches_per_probe = new UInt64[m_probing_depth];
        hits_per_probe_pt = new UInt64[m_probing_depth];
        prefetches_per_probe_pt = new UInt64[m_probing_depth];
        filtered_predictions_per_probe = new UInt64[m_probing_depth];

        for (int i = 0; i < m_probing_depth; i++)
        {
            relevator_prefetches = 0;
            hits_per_probe[i] = 0;
            prefetches_per_probe[i] = 0;
            hits_per_probe_pt[i] = 0;
            prefetches_per_probe_pt[i] = 0;
            filtered_predictions_per_probe[i] = 0;

            // registering stats
            registerStatsMetric("revelator", core->getId(), "hits_probe_" + itostr(i), &hits_per_probe[i]);
            registerStatsMetric("revelator", core->getId(), "prefetches_probe_" + itostr(i), &prefetches_per_probe[i]);
            registerStatsMetric("revelator", core->getId(), "hits_pt_probe_" + itostr(i), &hits_per_probe_pt[i]);
            registerStatsMetric("revelator", core->getId(), "prefetches_pt_probe_" + itostr(i), &prefetches_per_probe_pt[i]);
            registerStatsMetric("revelator", core->getId(), "filtered_predictions_probe_" + itostr(i), &filtered_predictions_per_probe[i]);
        }
        registerStatsMetric("revelator", core->getId(), "total_prefetches", &relevator_prefetches);
    }

    void RevelatorOpenAddressingEngine::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
#ifdef DEBUG_REVELATOR
        log_file << "[Revelator] Invoking Revelator with address " << address << std::endl;
#endif

        if (page_table_speculation && rev_type == 1)
            return;

        if (!page_table_speculation && rev_type == 2)
            return;

        if (oracle_revelator)
        {
            IntPtr cache_address = ((IntPtr)(physical_address)) & (~((64 - 1)));
            memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE)->handleMMUPrefetch(eip, cache_address, invoke_start_time);
        }
        else
        {
            // get predicted addresses using the new open addressing predict method
            std::vector<IntPtr> predictions = predict(address, page_table_speculation);
            relevator_prefetches++;

            for (size_t k = 0; k < predictions.size(); k++)
            {
                IntPtr cache_address = ((IntPtr)(predictions[k])) & (~((64 - 1)));

#ifdef DEBUG_REVELATOR
                log_file << "[Revelator] Probe: " << k << " Predicted PA: " << std::hex << cache_address << std::dec << std::endl;
#endif
                if (has_filter)
                {
                    // This filtering logic remains, but now 'k' corresponds to the probe depth
                    if (k > 0)
                    {
                        // Assuming the allocator has a compatible function for probe hit ratios
                        if (Sim()->getMimicOS()->getMemoryAllocator()->getAllocRatioForHash(static_cast<int>(k)) < 0.1f)
                        {
                            filtered_predictions_per_probe[k]++;
                            continue;
                        }
                    }
                }

                if (perfect_filtering && predictions[k] != physical_address)
                {
                    continue;
                }

                memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE)->handleMMUPrefetch(eip, cache_address, invoke_start_time);

                if (page_table_speculation)
                {
                    prefetches_per_probe_pt[k]++;
                }
                else
                {
                    prefetches_per_probe[k]++;
                }

                if (predictions[k] == physical_address)
                {
                    if (page_table_speculation)
                    {
                        hits_per_probe_pt[k]++;
                    }
                    else
                    {
                        hits_per_probe[k]++;
                    }
                }
            }
        }
    }

    UInt64 RevelatorOpenAddressingEngine::hashFunction(IntPtr address, int table_size)
    {
        uint64 result = CityHash64((const char *)&address, 8) % table_size;
        return result;
    }

    std::vector<IntPtr> RevelatorOpenAddressingEngine::predict(IntPtr address, bool is_page_table)
    {
        std::vector<IntPtr> predictions;
        // 1. Determine the value to hash and the offset within the page
        UInt64 to_hash = (is_page_table) ? address >> 21 : address >> 12;
        UInt64 offset = (is_page_table) ? ((address >> 12) & 0x1ff) * 8 : address & 0xFFF;

#ifdef DEBUG_REVELATOR
        log_file << "[Revelator] ToHash: " << to_hash << " VA: " << address << " is_pt: " << is_page_table << std::endl;
#endif

        // 2. Calculate the initial hash to get the starting page index
        UInt64 initial_page_index = hashFunction(to_hash, m_total_pages);

        // 3. Perform linear probing to generate predictions
        for (int d = 0; d < m_probing_depth; d++)
        {
            // Get the current page index by probing linearly from the start
            UInt64 current_page_index = (initial_page_index + d) % m_total_pages;

            // Construct the predicted physical address
            // Start with the physical page number (PPN)
            UInt64 predicted_pa = (current_page_index + m_kernel_pages);
            // Shift to get the page address
            predicted_pa = predicted_pa << 12;
            // Add the original offset
            predicted_pa = predicted_pa + offset;
            
            predictions.push_back(predicted_pa);
        }

        return predictions;
    }
}