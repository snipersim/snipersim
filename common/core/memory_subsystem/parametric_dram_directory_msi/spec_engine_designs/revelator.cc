#include "revelator.h"
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
    Revelator::Revelator(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name) : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name), name(_name), m_disabled(false), memory_manager(_memory_manager)
    {
#ifdef DEBUG_REVELATOR
        std::cout << "[Revelator] Initializing Revelator" << std::endl;
#endif
        // Getting config values
        number_of_hashes = Sim()->getCfg()->getInt("perf_model/revelator/number_of_hashes");           // number of hashes
        number_of_predictions = Sim()->getCfg()->getInt("perf_model/revelator/number_of_predictions"); // number of predictions
        m_memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/revelator/memory_size");           // total memory size
        kernel_size = Sim()->getCfg()->getInt("perf_model/revelator/kernel_size");                     // total kernel size
        oracle_revelator = Sim()->getCfg()->getBool("perf_model/revelator/oracle");                    // whether we are doing perfect refresh
        rev_type  = Sim()->getCfg()->getInt("perf_model/revelator/type");                     // rev type 0=full 1=data 2=translation only
        has_filter= Sim()->getCfg()->getBool("perf_model/revelator/filter"); 
        perfect_filtering = Sim()->getCfg()->getBool("perf_model/revelator/perfect_filtering"); // whether we are doing perfect refresh
        // oracle_revelator_filter = Sim()->getCfg()->getBool("perf_model/revelator/oracle_filter");
        m_total_pages = m_memory_size * 1024 / 4 - kernel_size * 1024 / 4; // calculating total pages
        m_kernel_pages = kernel_size * 1024 / 4;                           // calculating kernel pages

        // possible bloom filters
        //  bloom_parameters parameters;

        // revelator
        String allocator_name = Sim()->getMimicOS()->getMemoryAllocator()->getName();
        if (allocator_name != "revelator" && allocator_name != "revelator_simple")
        {
            // Phase 1a fullstack composition: Revelator engine constructed
            // alongside an incompatible allocator (e.g., reserve_thp, utopia).
            // Soft-disable: keep the engine object alive but make every
            // method a no-op so the diamond cells can run.  Phase 1b's
            // way-prediction integration replaces this stub with proper
            // Revelator-on-Utopia behaviour.
            std::cout << "[Revelator] Memory allocator is not revelator or revelator_simple (got: "
                      << allocator_name << ") — engine soft-disabled." << std::endl;
            m_disabled = true;
            return;
        }

        log_file_name = std::string(name.c_str()) + ".revelator.log";
        log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
        log_file.open(log_file_name);

        // Initialize timing experiment members
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;
        m_late_mispredict_data = 0;
        m_late_mispredict_pt = 0;

        // initializaing stats
        hits_per_hash = new UInt64[number_of_predictions];
        prefetches_per_hash = new UInt64[number_of_predictions];
        hits_per_hash_pt = new UInt64[number_of_predictions];
        prefetches_per_hash_pt = new UInt64[number_of_predictions];
        filtered_predictions_per_hash = new UInt64[number_of_predictions];

        for (int i = 0; i < number_of_predictions; i++)
        {

            relevator_prefetches = 0;
            hits_per_hash[i] = 0;
            prefetches_per_hash[i] = 0;
            hits_per_hash_pt[i] = 0;
            prefetches_per_hash_pt[i] = 0;
            filtered_predictions_per_hash[i]=0;

            // registering stats
            registerStatsMetric("revelator", core->getId(), "hits_" + itostr(i), &hits_per_hash[i]);
            registerStatsMetric("revelator", core->getId(), "prefetches_" + itostr(i), &prefetches_per_hash[i]);
            registerStatsMetric("revelator", core->getId(), "hits_pt_" + itostr(i), &hits_per_hash_pt[i]);
            registerStatsMetric("revelator", core->getId(), "prefetches_pt_" + itostr(i), &prefetches_per_hash_pt[i]);
            registerStatsMetric("revelator", core->getId(), "filtered_predictions_" + itostr(i), &filtered_predictions_per_hash[i]);

        }
        registerStatsMetric("revelator", core->getId(), "total_prefetches", &relevator_prefetches);
        registerStatsMetric("revelator", core->getId(), "late_mispredict_data", &m_late_mispredict_data);
        registerStatsMetric("revelator", core->getId(), "late_mispredict_pt", &m_late_mispredict_pt);
    }

    void Revelator::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
        // Reset per-invocation timing tracking
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;

        // Soft-disabled mode (Phase 1a): incompatible allocator → no-op.
        if (m_disabled) return;

#ifdef DEBUG_REVELATOR
        log_file << "[Revelator] Invoking Revelator with address " << address << std::endl;
#endif

        if(page_table_speculation && rev_type==1)
            return;

        if(!page_table_speculation && rev_type==2)
            return;

        // Get L2 cache interface for issuing prefetches and reading completion times
        auto *l2_cntlr = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);

        // Use correct block_type so prefetches land in the right MSHR map
        CacheBlockInfo::block_type_t spec_block_type = page_table_speculation
            ? CacheBlockInfo::block_type_t::PAGE_TABLE_DATA
            : CacheBlockInfo::block_type_t::DATA;

        if (oracle_revelator)
        {
            // oracle revelator implementation
            IntPtr cache_address = ((IntPtr)(physical_address)) & (~((64 - 1)));
            l2_cntlr->handleMMUPrefetch(eip, cache_address, invoke_start_time, spec_block_type);
            m_last_spec_completion = l2_cntlr->getLastPrefetchCompletion();
            m_last_prediction_correct = true;
        }
        else
        {
            // get predicted addresses
            std::vector<IntPtr> predictions = predict(address, number_of_predictions, page_table_speculation);
            relevator_prefetches++;

            for (size_t k = 0; k < predictions.size(); k++)
            {
                IntPtr cache_address = ((IntPtr)(predictions[k])) & (~((64 - 1)));

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
                
                // do prefetch for each hash
                l2_cntlr->handleMMUPrefetch(eip, cache_address, invoke_start_time, spec_block_type);

                // Capture prefetch completion time
                SubsecondTime this_prefetch_completion = l2_cntlr->getLastPrefetchCompletion();

                // Track the earliest prefetch completion (first hash to arrive)
                if (m_last_spec_completion == SubsecondTime::Zero() || this_prefetch_completion < m_last_spec_completion)
                    m_last_spec_completion = this_prefetch_completion;

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
                    m_last_prediction_correct = true;
                    // For correct prediction, use its specific completion time
                    m_last_spec_completion = this_prefetch_completion;

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
    UInt64 Revelator::hashFunction(IntPtr address, int table_size)
    {
        uint64 result = CityHash64((const char *)&address, 8) % table_size;
        return result;
    }
    std::vector<IntPtr> Revelator::predict(IntPtr address, int number_of_predictions, bool is_page_table)
    {

        std::vector<IntPtr> predictions;
        // finding hash and offset depending on whether or not the frame is for data or pt
        UInt64 to_hash = (is_page_table) ? address >> 21 : address >> 12;
        
#ifdef DEBUG_REVELATOR
        log_file<< "[Revelator] Hash: " << to_hash << " Address: " << address << std::endl;
#endif


        UInt64 offset = (is_page_table) ? ((address >> 12) & 0x1ff) * 8 : address & 0xFFF;

        if (number_of_predictions > number_of_hashes)
        {
            std::cout << "[VirtuOS:RevelatorPredictor] Number of predictions exceeds number of hashes." << std::endl;
            exit(1);
        }

        for (int i = 0; i < (number_of_hashes); i++)
        {
            UInt64 hash = hashFunction(to_hash * (i + 1), m_total_pages);
            hash = hash + m_kernel_pages;
            hash = hash << 12;
            hash = hash + offset;
            predictions.push_back(hash);
        }

        return predictions;
    }
}