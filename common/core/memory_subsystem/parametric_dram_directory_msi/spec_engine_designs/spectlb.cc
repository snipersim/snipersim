#include "spectlb.h"
#include "simulator.h"
#include "cache_cntlr.h"
#include "memory_manager.h"
#include "stats.h"
#include "mimicos.h"
#include "dvfs_manager.h"
#include "debug_config.h"
#include "memory_management/policies/buddy_policy.h"
#include "memory_management/policies/reserve_thp_policy.h"
#include "memory_management/physical_memory_allocators/reserve_thp.h"

namespace ParametricDramDirectoryMSI
{
    typedef ReservationTHPAllocator<Sniper::ReserveTHP::MetricsPolicy> SpecTLBReserveTHPAllocator;

    SpecTLB::SpecTLB(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name)
        : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name),
          spec_access_time(SubsecondTime::Zero()),
          spec_hit(false),
          spec_tlb_block_info(NULL),
          m_last_spec_completion(SubsecondTime::Zero()),
          m_last_prediction_correct(false)
    {
        m_log = new SimLog("SPEC_TLB", core->getId(), DEBUG_MMU_SPEC);
        m_log->log("Initializing SpecTLB");

        String tlbconfigstring = "perf_model/spec_tlb";
        String type = Sim()->getCfg()->getString(tlbconfigstring + "/type");
        int size = Sim()->getCfg()->getInt(tlbconfigstring + "/size");
        int assoc = Sim()->getCfg()->getInt(tlbconfigstring + "/assoc");
        int page_sizes = Sim()->getCfg()->getInt(tlbconfigstring + "/page_size");

        int *page_size_list = new int[page_sizes];
        for (int i = 0; i < page_sizes; i++)
            page_size_list[i] = Sim()->getCfg()->getIntArray(tlbconfigstring + "/page_size_list", i);

        ComponentLatency latency = ComponentLatency(
            core ? core->getDvfsDomain() : Sim()->getDvfsManager()->getGlobalDomain(DvfsManager::DvfsGlobalDomain::DOMAIN_GLOBAL_DEFAULT),
            Sim()->getCfg()->getInt(tlbconfigstring + "/access_latency"));

        // Its statistics register under "spec_tlb" (accesses, hits, misses, ...).
        spec_tlb = new TLB("spec_tlb", tlbconfigstring, core ? core->getId() : 0, latency, size, assoc,
                           page_size_list, page_sizes, type, false /* allocate_on_miss */);

        stats.invocations = 0;
        stats.hits = 0;
        stats.correct_predictions = 0;
        stats.allocations = 0;
        registerStatsMetric("spectlb", core->getId(), "invocations", &stats.invocations);
        registerStatsMetric("spectlb", core->getId(), "hits", &stats.hits);
        registerStatsMetric("spectlb", core->getId(), "correct_predictions", &stats.correct_predictions);
        registerStatsMetric("spectlb", core->getId(), "allocations", &stats.allocations);
    }

    SpecTLB::~SpecTLB()
    {
        delete spec_tlb;
        delete m_log;
    }

    void SpecTLB::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;

        // Data only: nothing is speculated about the page table itself.
        if (page_table_speculation)
            return;

        stats.invocations++;
        spec_hit = false;
        spec_access_time = invoke_start_time;
        spec_tlb_block_info = spec_tlb->lookup(address, spec_access_time, count, lock, eip, modeled, count, NULL);

        if (spec_tlb_block_info == NULL)
        {
            m_log->debug("SpecTLB miss for address ", SimLog::hex(address));
            return;
        }

        spec_hit = true;
        stats.hits++;

        // The region's base frame plus the offset within 2 MB: the physical
        // address the page has if it sits in the reserved region.
        const IntPtr base_page_size = 1 << 12;
        const IntPtr large_page_size = 1 << 21;
        IntPtr address_to_prefetch = (spec_tlb_block_info->getPPN() * base_page_size + address % large_page_size) & ~((IntPtr)(64 - 1));

        m_log->debug("SpecTLB hit for address ", SimLog::hex(address), ": region ppn ", spec_tlb_block_info->getPPN(),
                     ", prefetching ", SimLog::hex(address_to_prefetch));

        auto *l2_cntlr = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);
        l2_cntlr->handleMMUPrefetch(eip, address_to_prefetch, spec_access_time + spec_tlb->getLatency(), CacheBlockInfo::block_type_t::DATA);

        m_last_spec_completion = l2_cntlr->getLastPrefetchCompletion();
        m_last_prediction_correct = (address_to_prefetch == (physical_address & ~((IntPtr)(64 - 1))));
        if (m_last_prediction_correct)
            stats.correct_predictions++;
    }

    void SpecTLB::allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled)
    {
        if (spec_hit)
            return;

        // Only a reserved 2 MB region is worth an entry: the allocator says
        // whether the page's region is one, and gives its base frame.
        auto *res_allocator = dynamic_cast<SpecTLBReserveTHPAllocator *>(Sim()->getMimicOS()->getMemoryAllocator());
        if (res_allocator == NULL)
            return;
        IntPtr region_ppn = res_allocator->isLargePageReserved(address);
        if (region_ppn == static_cast<IntPtr>(-1))
            return;

        m_log->debug("Allocating region of ", SimLog::hex(address), " in the SpecTLB with base ppn ", region_ppn);
        spec_tlb->allocate(address, spec_access_time, count, lock, 21, region_ppn);
        stats.allocations++;
    }
}
