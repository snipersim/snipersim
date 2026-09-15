#include "revelator_thp_engine.h"
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
#include "mimicos.h"
#include "dram_cntlr_interface.h"
#include "memory_manager.h"

//#define DEBUG_REVELATOR

namespace ParametricDramDirectoryMSI
{
    RevelatorTHP::RevelatorTHP(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name) : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name), name(_name), memory_manager(_memory_manager)
    {
#ifdef DEBUG_REVELATOR
        std::cout << "[RevelatorTHP] Initializing RevelatorTHP" << std::endl;
#endif
        // Getting config values
        number_of_hashes = Sim()->getCfg()->getInt("perf_model/revelator_thp/number_of_hashes");           // number of hashes
        number_of_predictions = Sim()->getCfg()->getInt("perf_model/revelator_thp/number_of_predictions"); // number of predictions
        m_memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/revelator_thp/memory_size");           // total memory size
        kernel_size = Sim()->getCfg()->getInt("perf_model/revelator_thp/kernel_size");                     // total kernel size
        oracle_revelator = Sim()->getCfg()->getBool("perf_model/revelator_thp/oracle");                    // whether we are doing perfect refresh
        rev_type  = Sim()->getCfg()->getInt("perf_model/revelator_thp/type");                     // rev type 0=full 1=data 2=translation only
        has_filter= Sim()->getCfg()->getBool("perf_model/revelator_thp/filter"); 
        perfect_filtering = Sim()->getCfg()->getBool("perf_model/revelator_thp/perfect_filtering"); // whether we are doing perfect refresh
        // oracle_revelator_filter = Sim()->getCfg()->getBool("perf_model/revelator/oracle_filter");
        m_total_pages = m_memory_size * 1024 / 4 - kernel_size * 1024 / 4; // calculating total pages
        m_kernel_pages = kernel_size * 1024 / 4;                           // calculating kernel pages
        m_num_2mb_regions = m_total_pages / 512;                            // number of 2MB regions
        m_spec_mode = SpeculationMode::BOTH;                                // default to predicting both 4KB and 2MB

        // possible bloom filters
        //  bloom_parameters parameters;

        // revelator
        if (Sim()->getMimicOS()->getMemoryAllocator()->getName() != "revelator_thp")
        {
            std::cout << "[RevelatorTHP] Error: RevelatorTHP Spec Engine can only be used with the revelator_thp memory allocator." << std::endl;
            exit(1);
        }

        log_file_name = std::string(name.c_str()) + ".revelator_thp.log";
        log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
        log_file.open(log_file_name);

        // Dynamically size the stats arrays based on the speculation mode
        int stat_array_size = number_of_predictions;
        stat_array_size *= 2;

        // initializaing stats
        hits_per_hash = new UInt64[stat_array_size];
        prefetches_per_hash = new UInt64[stat_array_size];
        hits_per_hash_pt = new UInt64[stat_array_size];
        prefetches_per_hash_pt = new UInt64[stat_array_size];
        filtered_predictions_per_hash = new UInt64[stat_array_size];

        for (int i = 0; i < stat_array_size; i++)
        {

            relevator_prefetches = 0;
            hits_per_hash[i] = 0;
            prefetches_per_hash[i] = 0;
            hits_per_hash_pt[i] = 0;
            prefetches_per_hash_pt[i] = 0;
            filtered_predictions_per_hash[i]=0;

            // registering stats
            // Give stats a more descriptive name if predicting both sizes
            String metric_prefix = (m_spec_mode == SpeculationMode::BOTH && i >= number_of_predictions) ? "_2MB" : "_4KB";
            if (m_spec_mode != SpeculationMode::BOTH) metric_prefix = "";

            registerStatsMetric("revelator", core->getId(), "hits" + metric_prefix + "_" + itostr(i), &hits_per_hash[i]);
            registerStatsMetric("revelator", core->getId(), "prefetches" + metric_prefix + "_" + itostr(i), &prefetches_per_hash[i]);
            registerStatsMetric("revelator", core->getId(), "hits_pt" + metric_prefix + "_" + itostr(i), &hits_per_hash_pt[i]);
            registerStatsMetric("revelator", core->getId(), "prefetches_pt" + metric_prefix + "_" + itostr(i), &prefetches_per_hash_pt[i]);
            registerStatsMetric("revelator", core->getId(), "filtered_predictions" + metric_prefix + "_" + itostr(i), &filtered_predictions_per_hash[i]);

        }
        registerStatsMetric("revelator", core->getId(), "total_prefetches", &relevator_prefetches);
        m_stats_deactivated_invocations = 0;
        registerStatsMetric("revelator", core->getId(), "deactivated_invocations", &m_stats_deactivated_invocations);
    }

    void RevelatorTHP::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
        if(m_spec_mode == SpeculationMode::DEACTIVATED)
        {
            m_stats_deactivated_invocations++;
            // If the speculation mode is deactivated, we do not perform any speculative actions
            return;
        }
#ifdef DEBUG_REVELATOR
        log_file << "[Revelator] Invoking Revelator with address " << address << " is_page_table: " << page_table_speculation << std::endl;
#endif
        CacheBlockInfo::block_type_t block_type = (page_table_speculation) ? CacheBlockInfo::PAGE_TABLE : CacheBlockInfo::DATA;

        if(page_table_speculation && rev_type==1)
            return;

        if(!page_table_speculation && rev_type==2)
            return;

        if (oracle_revelator)
        {
            // oracle revelator implementation
            IntPtr cache_address = ((IntPtr)(physical_address)) & (~((64 - 1)));
            memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE)->handleMMUPrefetch(eip, cache_address, invoke_start_time, block_type);
        }
        else
        {
            // get predicted addresses
            bool temp;
            std::vector<IntPtr> predictions = predict(address, number_of_predictions, page_table_speculation);
            relevator_prefetches++;

            for (int k = 0; k < (int)predictions.size(); k++)
            {
                IntPtr cache_address = ((IntPtr)(predictions[k])) & (~((64 - 1)));

                bool revelator_hashf_prediction;

#ifdef DEBUG_REVELATOR
                log_file << "[Revelator] Hash: " << k << " Address: " << cache_address << std::endl;
#endif
                if(has_filter){
                    float thresholds[6]={85416666.0f,42708333.0f,20500000.0f,10049019.0f,5024509.0f,2512254.0f};
                    if(k>0){

                        if(Sim()->getMimicOS()->getMemoryAllocator()->getAllocRatioForHash(k)<0.1f){
                            filtered_predictions_per_hash[k]++;
                            continue;}
                    }
                    ShmemPerf m_dummy_shmem_perf;
                    auto *pdm = static_cast<ParametricDramDirectoryMSI::MemoryManager *>(memory_manager);
                    SubsecondTime time_to_request=pdm->getDramCntlr()->getDramPerfModel()->getAccessLatencyUnmodelled(invoke_start_time,pdm->getDramCntlr()->getCacheBlockSize(),core->getId(),cache_address);
#ifdef DEBUG_REVELATOR
                    log_file << "[Revelator] Hash: " << k << " Address: " << cache_address << " Time to request: " << time_to_request.getNS() << " Threshold: " <<SubsecondTime::FSfromFloat(thresholds[k]).getNS() << std::endl;
#endif
                    if(time_to_request > SubsecondTime::FSfromFloat(thresholds[k])){
#ifdef DEBUG_REVELATOR
                        log_file << "[Revelator] Hash: " << k << " Address: " << cache_address << " filtered out" << std::endl;
#endif
                        filtered_predictions_per_hash[k]++;
                        continue;
                    }
                }

                if (perfect_filtering && predictions[k] != physical_address)
                {
                    // If we are using the perfect filter, we will only prefetch the correct hash
                    continue;
                }
                
                // Prefetch the address while the translation is being performed
                memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE)->handleMMUPrefetch(eip, cache_address, invoke_start_time, block_type);

                if (page_table_speculation)
                {
                    prefetches_per_hash_pt[k]++;
                }
                else
                {
                    prefetches_per_hash[k]++;
                }
#ifdef DEBUG_REVELATOR
                if (page_table_speculation)
                {
                    log_file << "Revelator prediction: " << std::hex << predictions[k] << " Physical Address:" << std::hex << physical_address << " Is PT:" << page_table_speculation << "\n";

                }
#endif
                // check if the prediction is correct - keep stats
                if (predictions[k] == physical_address)
                {
                    if (page_table_speculation)
                    {
                        hits_per_hash_pt[k]++;
                    }
                    else
                    {
                        hits_per_hash[k]++;
                    }
                }
            }
        }
    }
    UInt64 RevelatorTHP::hashFunction(IntPtr address, int table_size)
    {
        uint64 result = CityHash64((const char *)&address, 8) % table_size;
        return result;
    }
    // --- START MODIFIED FUNCTION ---
    std::vector<IntPtr> RevelatorTHP::predict(IntPtr address, int number_of_predictions, bool is_page_table)
    {
        std::vector<IntPtr> predictions;

        if (number_of_predictions > number_of_hashes)
        {
            std::cout << "[VirtuOS:RevelatorPredictor] Number of predictions exceeds number of hashes." << std::endl;
            exit(1);
        }

        // --- Case 1: Page Table Speculation (Always 4KB frames) ---
        if (is_page_table && (m_spec_mode == SpeculationMode::ONLY_4KB || m_spec_mode == SpeculationMode::BOTH))
        {
            UInt64 to_hash = address >> 21; // Hash based on the 2MB virtual region
            UInt64 offset = ((address >> 12) & 0x1ff) * 8; // Offset of the PTE within the PT frame

            for (int i = 0; i < number_of_hashes; i++)
            {
                UInt64 hash = hashFunction(to_hash * (i + 1), m_kernel_pages);
                // Page table pages are allocated from the first part of memory (not kernel space in this model)
                hash = hash << 12; // Convert PPN to base address
                hash = hash + offset;
                predictions.push_back(hash);
            }
        }
        // --- Case 2: Data Speculation (4KB, 2MB, or Both) ---
        else
        {
            // Mode 0 (4KB) or 2 (Both): Generate 4KB predictions
            if (m_spec_mode == SpeculationMode::ONLY_4KB || m_spec_mode == SpeculationMode::BOTH)
            {
                UInt64 to_hash_4kb = address >> 12;
                UInt64 offset_4kb = address & 0xFFF;

                for (int i = 0; i < number_of_hashes; i++)
                {
                    UInt64 hash = hashFunction(to_hash_4kb * (i + 1), m_total_pages);
                    hash = hash + m_kernel_pages; // Adjust for user space
                    hash = hash << 12;
                    hash = hash + offset_4kb;
                    predictions.push_back(hash);
                }
            }
            // Mode 1 (2MB) or 2 (Both): Generate 2MB predictions
            if (m_spec_mode == SpeculationMode::ONLY_2MB || m_spec_mode == SpeculationMode::BOTH)
            {
                UInt64 to_hash_2mb = address >> 21;
                UInt64 offset_2mb = address & 0x1FFFFF; // 21-bit offset for 2MB page
                UInt64 num_2mb_regions = m_total_pages / 512;

                for (int i = 0; i < number_of_hashes; i++)
                {
                    UInt64 region_idx = hashFunction(to_hash_2mb * (i + 1), num_2mb_regions);
                    UInt64 base_ppn = region_idx * 512; // Convert 2MB region index to 4KB page index
                    base_ppn += m_kernel_pages; // Adjust for user space
                    UInt64 hash = (base_ppn << 12) + offset_2mb;
                    predictions.push_back(hash);
                }
            }
        }

        return predictions;
    }
    // --- END MODIFIED FUNCTION ---
}