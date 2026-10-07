#include "mimicos.h"
#include "line_out.h"
#include "config.hpp"
#include "allocator_factory.h"
#include "dvfs_manager.h"
#include "simulator.h"
#include "hooks_manager.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "core.h"
#include "performance_model.h"
#include "lock.h"
#include "debug_config.h"

#include <iostream>
#include <sstream>

// Static member definitions (for backward compatibility)
std::unordered_map<std::string, uint64_t> MimicOS::protocol_codes_encode = {
    {"page_fault", 1},
    {"syscall", 2}
};

std::unordered_map<uint64_t, std::string> MimicOS::protocol_codes_decode = {
    {1, "page_fault"},
    {2, "syscall"}
};

// ============ Construction/Destruction ============

MimicOS::MimicOS(bool is_guest)
    : m_name(is_guest ? "mimicos_guest" : "mimicos_host")
    , m_is_guest(is_guest)
    , m_userspace_enabled(false)
    , m_memory_allocator(nullptr)
    , m_swap_cache(nullptr)
    , m_last_pf_caused_swapping(false)
    , m_page_fault_latency(nullptr, 0)
    , m_target_fragmentation(0.0)
{
    LineOut() << "[MimicOS] Initializing MimicOS: " << m_name << std::endl;
    
    // Open log file
    m_log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" 
                    + std::string(m_name.c_str()) + ".log";
    m_log.open(m_log_file_name);
    
    m_log << "[MimicOS] Initializing MimicOS with name: " << m_name << std::endl;
    if (m_is_guest) {
        m_log << "[MimicOS] Guest OS is enabled" << std::endl;
    } else {
        m_log << "[MimicOS] Host OS is enabled" << std::endl;
    }
    
    // Read configuration
    m_page_table_type = Sim()->getCfg()->getString("perf_model/" + m_name + "/page_table_type");
    m_page_table_name = Sim()->getCfg()->getString("perf_model/" + m_name + "/page_table_name");
    m_range_table_type = Sim()->getCfg()->getString("perf_model/" + m_name + "/range_table_type");
    m_range_table_name = Sim()->getCfg()->getString("perf_model/" + m_name + "/range_table_name");
    
    bool swap_enabled = Sim()->getCfg()->getBool("perf_model/" + m_name + "/swap_enabled");
    m_userspace_enabled = Sim()->getCfg()->getBool("general/enable_userspace_mimicos");

    /* Userspace MimicOS schedules on a different axis from Sniper's own
       scheduler.  Sniper's decides which core a thread runs on; MimicOS's
       decides which application's instruction stream a TraceThread reads.
       They do not talk to each other, and one assumption connects them: a
       core's event queue is chosen from the posting thread's current core,
       while the kernel waits on a fixed core.  If Sniper moves a thread
       between those two points, the event is delivered to a core nobody is
       polling.

       Today that cannot happen -- the userspace path runs one TraceThread,
       with the application's reader swapped onto the kernel's own thread, so
       Sniper's scheduler has nothing to move.  static, pinned and roaming all
       produce identical results.  It becomes reachable with multi-core kernel
       threads, where each core polls its own queue, so the mismatch is worth
       naming now rather than after it produces a hang nobody can place.

       A warning, not an error: the configurations that exist today are
       correct under any of these schedulers. */
    if (m_userspace_enabled)
    {
        String sched = Sim()->getCfg()->getString("scheduler/type");
        if (sched != "static" && sched != "sequential")
        {
            std::cerr << "[MimicOS] Note: userspace MimicOS with scheduler/type="
                      << sched << ", which can migrate threads between cores. "
                      << "Per-core event queues assume a thread stays put "
                      << "between posting and polling; that assumption only "
                      << "binds once kernel threads run per core. Prefer "
                      << "scheduler/type=static for userspace runs."
                      << std::endl;
        }
    }
    
#if DEBUG_MIMICOS >= DEBUG_BASIC
    m_log << "[MimicOS] Page Table Type: " << m_page_table_type << std::endl;
    m_log << "[MimicOS] Range Table Type: " << m_range_table_type << std::endl;
    m_log << "[MimicOS] Swap Enabled: " << (swap_enabled ? "true" : "false") << std::endl;
    m_log << "[MimicOS] Userspace Enabled: " << (m_userspace_enabled ? "true" : "false") << std::endl;
#endif
    
    // Create memory allocator (only in sniper-space mode)
    if (!m_userspace_enabled) {
        String allocator_name = Sim()->getCfg()->getString("perf_model/" + m_name + "/memory_allocator_name");
        m_target_fragmentation = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/target_fragmentation");
        
        m_memory_allocator.reset(AllocatorFactory::createAllocator(m_name));
        LineOut() << "[MimicOS] Memory Allocator of type " << allocator_name << " created." << std::endl;
        
        // Read fragmentation mode configuration
        String frag_mode_str = "ratio";  // Default to ratio-based
        if (Sim()->getCfg()->hasKey("perf_model/" + allocator_name + "/fragmentation_mode")) {
            frag_mode_str = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/fragmentation_mode");
        }
        
        if (frag_mode_str == "count") {
            // Count-based fragmentation: fragment to exact number of free 2MB pages
            UInt64 target_free_2mb = 0;
            if (Sim()->getCfg()->hasKey("perf_model/" + allocator_name + "/target_free_2mb_pages")) {
                target_free_2mb = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/target_free_2mb_pages");
            }
            LineOut() << "[MimicOS] Using count-based fragmentation with target " << target_free_2mb << " free 2MB pages" << std::endl;
            m_memory_allocator->fragment_memory_to_count(target_free_2mb);
        } else {
            // Ratio-based fragmentation (default): fragment to target ratio
            LineOut() << "[MimicOS] Using ratio-based fragmentation with target ratio " << m_target_fragmentation << std::endl;
            m_memory_allocator->fragment_memory(m_target_fragmentation);
        }
        
#if DEBUG_MIMICOS >= DEBUG_BASIC
        m_log << "[MimicOS] Memory Allocator created and fragmented" << std::endl;
#endif
    }
    
    // Page fault latency
    m_page_fault_latency = ComponentLatency(
        Sim()->getDvfsManager()->getGlobalDomain(),
        Sim()->getCfg()->getInt("perf_model/" + m_name + "/page_fault_latency"));
    
    m_log << "[MimicOS] Page fault latency: " << m_page_fault_latency.getLatency().getNS() << " ns" << std::endl;
    
    // Page sizes
    int num_page_sizes = Sim()->getCfg()->getInt("perf_model/" + m_name + "/number_of_page_sizes");
    m_page_sizes.reserve(num_page_sizes);
    
    for (int i = 0; i < num_page_sizes; i++) {
        m_page_sizes.push_back(Sim()->getCfg()->getIntArray("perf_model/" + m_name + "/page_size_list", i));
#if DEBUG_MIMICOS >= DEBUG_DETAILED
        m_log << "[MimicOS] Page size " << i << ": " << m_page_sizes[i] << " bytes" << std::endl;
#endif
    }
    
    // Swap cache
    if (swap_enabled) {
        m_swap_cache = std::make_unique<SwapCache>();
#if DEBUG_MIMICOS >= DEBUG_BASIC
        m_log << "[MimicOS] Swap cache created" << std::endl;
#endif
    }
    
    // HugeTLBfs service (only in sniper-space mode)
    if (!m_userspace_enabled) {
        m_hugetlbfs = std::make_unique<HugeTLBfs>();
        if (m_hugetlbfs->isEnabled()) {
#if DEBUG_MIMICOS >= DEBUG_BASIC
            m_log << "[MimicOS] HugeTLBfs service enabled with "
                  << m_hugetlbfs->getTotalHugePages(HugeTLBfs::HugePageSize::SIZE_2MB) << " 2MB pages and "
                  << m_hugetlbfs->getTotalHugePages(HugeTLBfs::HugePageSize::SIZE_1GB) << " 1GB pages" << std::endl;
#endif
            // The pool's frames are carved from the kernel-reserved region
            // of physical memory, which the allocator never hands out as
            // data pages and whose page-table frames grow up from its
            // start.  A pool that does not fit there would alias the
            // allocator's frames, so it is refused rather than mapped.
            if (m_memory_allocator) {
                IntPtr kernel_end_ppn = m_memory_allocator->getKernelSizeMB() << 8;   // MB -> 4 KB pages
                if (m_hugetlbfs->getPoolEndPPN() > kernel_end_ppn) {
                    std::cerr << "[FATAL] [MimicOS] hugetlbfs pool [ppn 0x" << std::hex
                              << m_hugetlbfs->getPoolBasePPN() << ", 0x" << m_hugetlbfs->getPoolEndPPN()
                              << std::dec << ") does not fit in the allocator's kernel region (kernel_size = "
                              << m_memory_allocator->getKernelSizeMB()
                              << " MB): reduce nr_hugepages_2mb/nr_hugepages_1gb" << std::endl;
                    exit(1);
                }
            }
        } else {
#if DEBUG_MIMICOS >= DEBUG_BASIC
            m_log << "[MimicOS] HugeTLBfs service disabled" << std::endl;
#endif
        }
    }
    

    // ---- Linux daemon models -------------------------------------------
    //
    // Each is off unless its configuration turns it on, and each runs from
    // HOOK_PERIODIC at its own simulated interval rather than on a host thread,
    // so an enabled daemon costs simulated time in the run it belongs to.
    // Individual parameters are read defensively: a daemon that is enabled but
    // under-specified falls back to its own defaults rather than failing the run.

    {   // kcompactd: memory compaction
        CompactdConfig cfg;
        cfg.enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + m_name + "/kcompactd_enabled", false);
        if (cfg.enabled)
        {
            cfg.scan_interval_us = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kcompactd_scan_interval_us", cfg.scan_interval_us);
            cfg.low_util_threshold = Sim()->getCfg()->getFloatDefault("perf_model/" + m_name + "/kcompactd_low_util_threshold", cfg.low_util_threshold);
            cfg.high_util_threshold = Sim()->getCfg()->getFloatDefault("perf_model/" + m_name + "/kcompactd_high_util_threshold", cfg.high_util_threshold);
            cfg.max_pages_per_scan = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kcompactd_max_pages_per_scan", cfg.max_pages_per_scan);

            m_kcompactd = std::make_unique<SniperKcompactd>(cfg);
            m_kcompactd_interval_ns = (UInt64)cfg.scan_interval_us * 1000;
            m_kcompactd_last_scan_ns = 0;

            // Compaction works on the 2MB map, which only the reservation
            // allocator maintains.
            auto* rthp = dynamic_cast<SniperReserveTHPAllocator*>(m_memory_allocator.get());
            if (rthp)
            {
                // bind only: start() would spawn a host pthread that scans in
                // wall-clock time, independently of the simulated-time hook
                // below, which both inflates the scan count and makes the run
                // depend on how fast the host is.
                m_kcompactd->bind(&rthp->getTwoMbMap(), rthp->getBuddyAllocator());
                Sim()->getHooksManager()->registerHook(
                    HookType::HOOK_PERIODIC, MimicOS::hookKcompactdPeriodic, (UInt64)this);
            }
            else
            {
                std::cerr << "[MimicOS] kcompactd is enabled but the allocator is not reserve_thp; "
                             "it has no 2MB map to compact and stays idle" << std::endl;
                m_kcompactd.reset();
            }
        }
    }

    {   // khugepaged: THP coalescing
        KhugepagedConfig cfg;
        cfg.enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + m_name + "/khugepaged_enabled", false);
        if (cfg.enabled)
        {
            cfg.scan_interval_us = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/khugepaged_scan_interval_us", cfg.scan_interval_us);
            cfg.promotion_threshold = Sim()->getCfg()->getFloatDefault("perf_model/" + m_name + "/khugepaged_promotion_threshold", cfg.promotion_threshold);
            cfg.max_promotions_per_scan = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/khugepaged_max_promotions_per_scan", cfg.max_promotions_per_scan);

            m_khugepaged = std::make_unique<SniperKhugepaged>(cfg);
            m_khugepaged_interval_ns = (UInt64)cfg.scan_interval_us * 1000;
            m_khugepaged_last_scan_ns = 0;

            auto* rthp = dynamic_cast<SniperReserveTHPAllocator*>(m_memory_allocator.get());
            if (rthp)
            {
                m_khugepaged->bind(&rthp->getTwoMbMap());
                Sim()->getHooksManager()->registerHook(
                    HookType::HOOK_PERIODIC, MimicOS::hookKhugepagedPeriodic, (UInt64)this);
            }
            else
            {
                std::cerr << "[MimicOS] khugepaged is enabled but the allocator is not reserve_thp; "
                             "it has nothing to promote and stays idle" << std::endl;
                m_khugepaged.reset();
            }
        }
    }

    {   // kswapd: page reclamation
        KswapdConfig cfg;
        cfg.enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + m_name + "/kswapd_enabled", false);
        if (cfg.enabled && m_memory_allocator)
        {
            cfg.scan_interval_us = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kswapd_scan_interval_us", cfg.scan_interval_us);
            cfg.pages_high = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kswapd_pages_high", cfg.pages_high);
            cfg.pages_low = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kswapd_pages_low", cfg.pages_low);
            cfg.pages_min = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kswapd_pages_min", cfg.pages_min);
            cfg.batch_size = Sim()->getCfg()->getIntDefault("perf_model/" + m_name + "/kswapd_batch_size", cfg.batch_size);

            m_kswapd = std::make_unique<SniperKswapd>(cfg);
            m_kswapd_interval_ns = (UInt64)cfg.scan_interval_us * 1000;
            m_kswapd_last_scan_ns = 0;
            // No free-page source: bind() takes a pointer to the allocator's
            // free-page count, and no allocator here exposes one.  Virtuoso
            // passes nullptr too, and the consequence is visible in the
            // statistics -- with free_pages reading zero and nothing on the LRU
            // to reclaim, kswapd records an oom_event on every scan.  It is
            // ported so the userspace MimicOS work has it, but it models
            // nothing until an allocator offers a free-page count.
            m_kswapd->bind(nullptr, 0);
            std::cerr << "[MimicOS] kswapd enabled, but no allocator here exposes a free-page "
                         "count; it will scan and report OOM without reclaiming" << std::endl;
            Sim()->getHooksManager()->registerHook(
                HookType::HOOK_PERIODIC, MimicOS::hookKswapdPeriodic, (UInt64)this);
        }
    }

    LineOut() << "[MimicOS] Initialization complete" << std::endl;
}

MimicOS::~MimicOS()
{
#if DEBUG_MIMICOS >= DEBUG_BASIC
    m_log << "[MimicOS] Destructor called" << std::endl;
#endif
    
    // Dump allocator final stats before destruction
    if (m_memory_allocator) {
        m_memory_allocator->dumpFinalStats();
    }
    
    // Smart pointers handle cleanup automatically
    m_applications.clear();
    
    if (m_log.is_open()) {
        m_log.close();
    }
}

// ============ Application Management ============

void MimicOS::createApplication(int app_id)
{
#if DEBUG_MIMICOS >= DEBUG_BASIC
    m_log << "[MimicOS] Creating application " << app_id << std::endl;
#endif
    
    if (m_applications.find(app_id) != m_applications.end()) {
        m_log << "[MimicOS] Application " << app_id << " already exists" << std::endl;
        return;
    }
    
    // Create application context
    auto app = std::make_unique<ApplicationContext>(
        app_id,
        m_page_table_type,
        m_page_table_name,
        m_range_table_type,
        m_range_table_name,
        m_is_guest);
    
    // Parse VMAs from trace file
    String app_id_str = std::to_string(app_id).c_str();
    
    if (Sim()->getCfg()->hasKey("traceinput/thread_" + app_id_str)) {
        String trace_file = Sim()->getCfg()->getString("traceinput/thread_" + app_id_str);
        app->parseVMAsFromFile(std::string(trace_file.c_str()));
        m_log << "[MimicOS] VMAs for application " << app_id << " have been parsed" << std::endl;
    } else {
        m_log << "[MimicOS] No VMA file provided for application " << app_id << std::endl;
    }
    
    m_applications[app_id] = std::move(app);
    
#if DEBUG_MIMICOS >= DEBUG_BASIC
    m_log << "[MimicOS] Application " << app_id << " created successfully" << std::endl;
#endif
}

ApplicationContext* MimicOS::getApplication(int app_id)
{
    auto it = m_applications.find(app_id);
    if (it != m_applications.end()) {
        return it->second.get();
    }
    return nullptr;
}

const ApplicationContext* MimicOS::getApplication(int app_id) const
{
    auto it = m_applications.find(app_id);
    if (it != m_applications.end()) {
        return it->second.get();
    }
    return nullptr;
}

// ============ Page Table Access ============

ParametricDramDirectoryMSI::PageTable* MimicOS::getPageTable(int app_id)
{
    auto* app = getApplication(app_id);
    return app ? app->getPageTable() : nullptr;
}

ParametricDramDirectoryMSI::RangeTable* MimicOS::getRangeTable(int app_id)
{
    auto* app = getApplication(app_id);
    return app ? app->getRangeTable() : nullptr;
}

// ============ VMA Access ============

std::vector<VMA>& MimicOS::getVMA(int app_id)
{
    auto* app = getApplication(app_id);
    if (app) {
        return app->getVMAs();
    }
    // Return empty static vector as fallback (shouldn't happen in normal use)
    static std::vector<VMA> empty;
    return empty;
}

void MimicOS::setAllocatedVMA(int app_id, int vma_index)
{
    auto* app = getApplication(app_id);
    if (app) {
        app->setVMAAllocated(static_cast<size_t>(vma_index));
    }
}

void MimicOS::setPhysicalOffset(int app_id, int vma_index, IntPtr offset)
{
    auto* app = getApplication(app_id);
    if (app) {
        app->setVMAPhysicalOffset(static_cast<size_t>(vma_index), offset);
    }
}

IntPtr MimicOS::getPhysicalOffsetSpot(int app_id, IntPtr va)
{
    auto* app = getApplication(app_id);
    if (app) {
        return app->getPhysicalOffset(va);
    }
    return static_cast<IntPtr>(-1);
}

bool MimicOS::incrementSuccessfulOffsetBasedAllocations(int app_id, IntPtr va)
{
    auto* app = getApplication(app_id);
    if (app) {
        return app->incrementOffsetAllocations(va);
    }
    return false;
}

int MimicOS::getSuccessfulOffsetBasedAllocations(int app_id, IntPtr va)
{
    auto* app = getApplication(app_id);
    if (app) {
        return app->getOffsetAllocations(va);
    }
    return -1;
}

int MimicOS::getVMAThresholdSpot(int app_id, IntPtr va)
{
    // This seems to be the same as getSuccessfulOffsetBasedAllocations
    return getSuccessfulOffsetBasedAllocations(app_id, va);
}

UInt64 MimicOS::getAccessesPerVPN(IntPtr vpn, int app_id)
{
    auto* app = getApplication(app_id);
    if (app) {
        return app->getAccessesPerVPN(vpn);
    }
    return 0;
}

// ============ Swap Management ============

bool MimicOS::swapOutPage(IntPtr vpn, int app_id)
{
    if (m_swap_cache) {
#if DEBUG_MIMICOS >= DEBUG_BASIC
        m_log << "[MimicOS] Swapping out page: " << vpn << " for app_id: " << app_id << std::endl;
#endif
        bool is_memory_full = false;
        return m_swap_cache->swapOut(vpn, app_id, is_memory_full);
    }
    return false;
}

void MimicOS::deletePageTableEntry(IntPtr victim_address, int app_id)
{
#if DEBUG_MIMICOS >= DEBUG_BASIC
    m_log << "[MimicOS] Deleting page table entry for address: 0x" << std::hex << victim_address 
          << " app_id: " << std::dec << app_id << std::endl;
#endif
    
    auto* app = getApplication(app_id);
    if (app) {
        app->deletePageTableEntry(victim_address);
    } else {
        m_log << "[MimicOS] Application " << app_id << " does not exist" << std::endl;
    }
}

// ============ HugeTLBfs Management ============

IntPtr MimicOS::hugePageBlockFor(int app_id, IntPtr vaddr)
{
    if (!m_hugetlbfs || !m_hugetlbfs->isEnabled() || !m_hugetlbfs->hasBackingCriterion())
        return static_cast<IntPtr>(-1);

    auto* app = getApplication(app_id);
    if (!app || app->getVMAs().empty())
        return static_cast<IntPtr>(-1);

    const IntPtr block_base = vaddr & ~((static_cast<IntPtr>(1) << HUGEPAGE_2MB_BITS) - 1);
    const IntPtr block_end = block_base + (static_cast<IntPtr>(1) << HUGEPAGE_2MB_BITS);

    auto& blocks = m_hugetlb_blocks[app_id];
    auto it = blocks.find(block_base);
    bool backed;
    if (it != blocks.end()) {
        backed = it->second;
    } else {
        backed = m_hugetlbfs->backs(block_base, block_end, app->getVMAs());
        blocks[block_base] = backed;
#if DEBUG_MIMICOS >= DEBUG_BASIC
        m_log << "[MimicOS] HugeTLBfs " << (backed ? "backs" : "does not back")
              << " block 0x" << std::hex << block_base << std::dec << " of app " << app_id << std::endl;
#endif
    }
    return backed ? block_base : static_cast<IntPtr>(-1);
}

IntPtr MimicOS::allocateHugePage(int app_id, IntPtr vaddr, bool size_2mb)
{
    if (!m_hugetlbfs || !m_hugetlbfs->isEnabled()) {
#if DEBUG_MIMICOS >= DEBUG_BASIC
        m_log << "[MimicOS] HugeTLBfs not available for huge page allocation" << std::endl;
#endif
        return static_cast<IntPtr>(-1);
    }
    
    // Check if the application has VMAs available (trace file was parsed)
    // HugeTLBfs should only be used when VMA information is present
    auto* app = getApplication(app_id);
    if (!app || app->getVMAs().empty()) {
#if DEBUG_MIMICOS >= DEBUG_BASIC
        m_log << "[MimicOS] HugeTLBfs: No VMAs available for app " << app_id 
              << ", skipping huge page allocation" << std::endl;
#endif
        return static_cast<IntPtr>(-1);
    }
    
    HugeTLBfs::HugePageSize size = size_2mb ? 
        HugeTLBfs::HugePageSize::SIZE_2MB : HugeTLBfs::HugePageSize::SIZE_1GB;
    
    IntPtr ppn = m_hugetlbfs->allocateHugePage(app_id, vaddr, size);
    
#if DEBUG_MIMICOS >= DEBUG_BASIC
    if (ppn != static_cast<IntPtr>(-1)) {
        m_log << "[MimicOS] Allocated " << (size_2mb ? "2MB" : "1GB") 
              << " huge page for app " << app_id 
              << " at vaddr 0x" << std::hex << vaddr 
              << " -> ppn 0x" << ppn << std::dec << std::endl;
    } else {
        m_log << "[MimicOS] Failed to allocate " << (size_2mb ? "2MB" : "1GB") 
              << " huge page for app " << app_id << std::endl;
    }
#endif
    
    return ppn;
}

bool MimicOS::deallocateHugePage(IntPtr base_ppn, bool size_2mb)
{
    if (!m_hugetlbfs || !m_hugetlbfs->isEnabled()) {
        return false;
    }
    
    HugeTLBfs::HugePageSize size = size_2mb ? 
        HugeTLBfs::HugePageSize::SIZE_2MB : HugeTLBfs::HugePageSize::SIZE_1GB;
    
    bool success = m_hugetlbfs->deallocateHugePage(base_ppn, size);
    
#if DEBUG_MIMICOS >= DEBUG_BASIC
    m_log << "[MimicOS] Deallocate " << (size_2mb ? "2MB" : "1GB") 
          << " huge page at ppn 0x" << std::hex << base_ppn << std::dec
          << ": " << (success ? "success" : "failed") << std::endl;
#endif
    
    return success;
}

// ============ Per-Core Statistics ============

// Static empty stats for invalid core_id
PerCoreStats MimicOS::s_empty_stats;

void MimicOS::initPerCoreStats(UInt32 num_cores)
{
    if (m_per_core_stats.empty()) {
        m_per_core_stats.resize(num_cores);
        m_log << "[MimicOS] Initialized per-core stats for " << num_cores << " cores" << std::endl;
    }
}

void MimicOS::initPerCorePageFaultState(UInt32 num_cores)
{
    if (m_pf_states.empty()) {
        m_pf_states.resize(num_cores);
    }
    /* Per-core event queues, one deque and one mutex each.  Sized here
       rather than in the constructor because the core count is not known
       until the CoreManager has built them. */
    if (m_event_queues.empty()) {
        m_event_queues.resize(num_cores);
        m_idle_waiters.assign(num_cores, INVALID_THREAD_ID);
        m_event_queue_locks.reserve(num_cores);
        for (UInt32 i = 0; i < num_cores; ++i) {
            m_event_queue_locks.emplace_back(new std::mutex());
        }
    }
}

const PerCoreStats& MimicOS::getPerCoreStats(core_id_t core_id) const
{
    if (core_id >= 0 && (size_t)core_id < m_per_core_stats.size()) {
        return m_per_core_stats[core_id];
    }
    return s_empty_stats;
}

PerCoreStats* MimicOS::getPerCoreStatsMutable(core_id_t core_id)
{
    if (core_id >= 0 && (size_t)core_id < m_per_core_stats.size()) {
        return &m_per_core_stats[core_id];
    }
    return nullptr;
}

void MimicOS::updateTranslationStats(core_id_t core_id, 
                                     SubsecondTime tr_latency,
                                     bool is_l2_miss,
                                     SubsecondTime walk_latency)
{
    PerCoreStats* stats = getPerCoreStatsMutable(core_id);
    if (stats) {
        stats->translation_latency += tr_latency;
        stats->num_translations++;
        if (is_l2_miss) {
            stats->l2_tlb_misses++;
            stats->page_walk_latency += walk_latency;
        } else {
            stats->l2_tlb_hits++;
        }
    }
}

void MimicOS::updateInstructionStats(core_id_t core_id, UInt64 instructions, UInt64 cycles)
{
    PerCoreStats* stats = getPerCoreStatsMutable(core_id);
    if (stats) {
        stats->instructions_executed = instructions;
        stats->cycles = cycles;
    }
}

void MimicOS::updateDataAccessStats(core_id_t core_id, SubsecondTime latency, bool is_cache_hit)
{
    PerCoreStats* stats = getPerCoreStatsMutable(core_id);
    if (stats) {
        stats->data_access_latency += latency;
        stats->num_data_accesses++;
        if (is_cache_hit) {
            stats->cache_hits++;
        } else {
            stats->cache_misses++;
        }
    }
}

/* ---- idle-worker wait ------------------------------------------------- */

void MimicOS::registerIdleWaiter(int core_id, thread_id_t thread_id)
{
    if (core_id >= 0 && (size_t)core_id < m_idle_waiters.size())
        m_idle_waiters[core_id] = thread_id;
}

void MimicOS::clearIdleWaiter(int core_id)
{
    if (core_id >= 0 && (size_t)core_id < m_idle_waiters.size())
        m_idle_waiters[core_id] = INVALID_THREAD_ID;
}

/* The wake time is the poster's "now".  Posters are TraceThreads, which
   do not register a current core, so take the latest core time in the
   system: an interrupt arrives at global time, and the barrier keeps the
   cores within a skew of each other anyway. */
static SubsecondTime latestCoreTime()
{
    SubsecondTime t = SubsecondTime::Zero();
    core_id_t n = (core_id_t)Sim()->getConfig()->getApplicationCores();
    for (core_id_t c = 0; c < n; c++) {
        Core* core = Sim()->getCoreManager()->getCoreFromID(c);
        if (core && core->getPerformanceModel()->getElapsedTime() > t)
            t = core->getPerformanceModel()->getElapsedTime();
    }
    return t;
}

void MimicOS::wakeIdleWaiter(int core_id)
{
    ThreadManager* tm = Sim()->getThreadManager();
    if (!tm) return;
    ScopedLock sl(tm->getLock());
    wakeIdleWaiter_unlocked(core_id);
}

void MimicOS::wakeIdleWaiter_unlocked(int core_id)
{
    if (core_id < 0 || (size_t)core_id >= m_idle_waiters.size()) return;
    ThreadManager* tm = Sim()->getThreadManager();
    thread_id_t waiter = m_idle_waiters[core_id];
    if (waiter == INVALID_THREAD_ID) return;
    m_idle_waiters[core_id] = INVALID_THREAD_ID;   // one wake per registration
    tm->resumeThread(waiter, INVALID_THREAD_ID, latestCoreTime());
}

void MimicOS::wakeAllIdleWaiters()
{
    for (size_t c = 0; c < m_idle_waiters.size(); c++)
        wakeIdleWaiter((int)c);
}

void MimicOS::notifyEvent(int core_id, UInt64 kind, UInt64 arg)
{
    HooksManager::MimicOSEvent args;
    args.core_id = core_id;
    args.kind = kind;
    args.arg = arg;
    Core* core = Sim()->getCoreManager()->getCoreFromID(core_id);
    args.time = core ? core->getPerformanceModel()->getElapsedTime() : SubsecondTime::Zero();
    Sim()->getHooksManager()->callHooks(HookType::HOOK_MIMICOS_EVENT, (UInt64)&args);
}
