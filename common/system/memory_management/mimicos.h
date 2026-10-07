#ifndef MIMICOS_H
#define MIMICOS_H

#include "../../../include/memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "application_context.h"
#include "page_fault_state.h"
#include "mimicos_message.h"
#include "swap_space.h"
#include "memory_management/kcompactd.h"
#include "memory_management/policies/kcompactd_policy.h"
#include "memory_management/khugepaged.h"
#include "memory_management/policies/khugepaged_policy.h"
#include "memory_management/kswapd.h"
#include "memory_management/policies/kswapd_policy.h"
#include "hooks_manager.h"
#include "hugetlbfs/hugetlbfs.h"
#include "subsecond_time.h"
#include "fixed_types.h"

#include <unordered_map>
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <fstream>

// Backward compatibility: old code used MimicOS_NS::Message
namespace MimicOS_NS {
    using Message = MimicOSMessage;
}

/**
 * @brief Per-core performance statistics for adaptive policies
 * 
 * This structure holds per-core statistics that can be used by various
 * adaptive components (e.g., replacement controllers, prefetchers, etc.)
 * 
 * Updated by MMU on each translation, accessible globally via MimicOS.
 */
struct PerCoreStats {
    // ============ Translation Statistics ============
    SubsecondTime translation_latency;      // Cumulative translation latency
    SubsecondTime page_walk_latency;        // Cumulative page table walk latency
    UInt64 num_translations;                // Total translations performed
    UInt64 l2_tlb_misses;                   // L2 TLB misses (triggers page walks)
    UInt64 l2_tlb_hits;                     // L2 TLB hits
    
    // ============ Data Access Statistics ============
    SubsecondTime data_access_latency;      // Cumulative data access latency
    UInt64 num_data_accesses;               // Total data accesses
    UInt64 cache_hits;                      // Cache hits (L1/L2/L3)
    UInt64 cache_misses;                    // Cache misses (DRAM accesses)
    
    // ============ NUCA Cache Statistics ============
    UInt64 nuca_accesses;                   // Total NUCA accesses (reads + writes)
    UInt64 nuca_misses;                     // Total NUCA misses (read + write misses)
    UInt64 nuca_metadata_misses;            // NUCA misses for metadata (page table) blocks
    UInt64 nuca_data_misses;                // NUCA misses for data blocks
    
    // ============ Instruction Statistics ============
    UInt64 instructions_executed;           // Total instructions executed
    UInt64 cycles;                          // Total cycles
    
    // ============ Timing ============
    SubsecondTime last_update_time;         // When stats were last updated
    
    PerCoreStats() 
        : translation_latency(SubsecondTime::Zero())
        , page_walk_latency(SubsecondTime::Zero())
        , num_translations(0)
        , l2_tlb_misses(0)
        , l2_tlb_hits(0)
        , data_access_latency(SubsecondTime::Zero())
        , num_data_accesses(0)
        , cache_hits(0)
        , cache_misses(0)
        , nuca_accesses(0)
        , nuca_misses(0)
        , nuca_metadata_misses(0)
        , nuca_data_misses(0)
        , instructions_executed(0)
        , cycles(0)
        , last_update_time(SubsecondTime::Zero())
    {}
    
    // Derived metrics
    float getL2TlbMissRate() const {
        UInt64 total = l2_tlb_hits + l2_tlb_misses;
        return total > 0 ? (float)l2_tlb_misses / total : 0.0f;
    }
    
    float getL2TlbMPKI() const {
        return instructions_executed > 0 ? 
            (float)l2_tlb_misses * 1000.0f / instructions_executed : 0.0f;
    }
    
    float getTranslationShare() const {
        // Translation latency as fraction of total cycles
        if (cycles == 0) return 0.0f;
        return (float)translation_latency.getNS() / (float)cycles;
    }
    
    float getRho() const {
        // Ratio of translation stalls to data stalls
        if (data_access_latency.getNS() == 0) return 0.0f;
        return (float)translation_latency.getNS() / (float)data_access_latency.getNS();
    }
    
    float getNucaMPKI() const {
        // NUCA misses per 1000 instructions
        return instructions_executed > 0 ? 
            (float)nuca_misses * 1000.0f / instructions_executed : 0.0f;
    }
    
    float getNucaMetadataMPKI() const {
        // NUCA metadata misses per 1000 instructions
        return instructions_executed > 0 ? 
            (float)nuca_metadata_misses * 1000.0f / instructions_executed : 0.0f;
    }
    
    float getNucaDataMPKI() const {
        // NUCA data misses per 1000 instructions
        return instructions_executed > 0 ? 
            (float)nuca_data_misses * 1000.0f / instructions_executed : 0.0f;
    }
};

/**
 * @brief MimicOS - Simulated Operating System for memory management
 * 
 * MimicOS coordinates memory management simulation:
 * - Application lifecycle (page tables, VMAs)
 * - Physical memory allocation
 * - Swap space (optional)
 * - Communication with userspace MimicOS (VirtuOS)
 * 
 * Two modes of operation:
 * 1. Sniper-space: MimicOS handles page faults directly
 * 2. Userspace: MimicOS communicates with VirtuOS for page fault handling
 * 
 * This class uses modular components:
 * - ApplicationContext: per-application state (page tables, VMAs)
 * - PageFaultState: page fault tracking
 * - MimicOSMessage/Protocol: userspace communication
 */

// The Linux daemon models, bound to their Sniper metrics policies.
using SniperKcompactd  = ::Kcompactd<Sniper::Kcompactd::MetricsPolicy>;
using SniperKhugepaged = ::Khugepaged<Sniper::Khugepaged::MetricsPolicy>;
using SniperKswapd     = ::Kswapd<Sniper::Kswapd::MetricsPolicy>;

class MimicOS
{
public:
    // ============ Construction/Destruction ============
    
    /**
     * @brief Construct MimicOS
     * 
     * @param is_guest True if this is a guest OS (for virtualization)
     */
    explicit MimicOS(bool is_guest);
    ~MimicOS();
    
    // Disable copy
    MimicOS(const MimicOS&) = delete;
    MimicOS& operator=(const MimicOS&) = delete;

    // ============ Application Management ============
    
    /**
     * @brief Create a new application context
     * 
     * @param app_id Application ID
     */
    void createApplication(int app_id);
    
    /**
     * @brief Get the application context for an app
     * 
     * @param app_id Application ID
     * @return ApplicationContext* or nullptr if not found
     */
    ApplicationContext* getApplication(int app_id);
    const ApplicationContext* getApplication(int app_id) const;

    // ============ Page Table Access (convenience methods) ============
    
    ParametricDramDirectoryMSI::PageTable* getPageTable(int app_id);
    ParametricDramDirectoryMSI::RangeTable* getRangeTable(int app_id);
    
    // ============ VMA Access (convenience methods) ============
    
    std::vector<VMA>& getVMA(int app_id);
    
    void setAllocatedVMA(int app_id, int vma_index);
    void setPhysicalOffset(int app_id, int vma_index, IntPtr offset);
    IntPtr getPhysicalOffsetSpot(int app_id, IntPtr va);
    bool incrementSuccessfulOffsetBasedAllocations(int app_id, IntPtr va);
    int getSuccessfulOffsetBasedAllocations(int app_id, IntPtr va);
    int getVMAThresholdSpot(int app_id, IntPtr va);
    
    // ============ Memory Allocator ============
    
    PhysicalMemoryAllocator* getMemoryAllocator() { return m_memory_allocator.get(); }

    // ============ OS daemons ============
    SniperKcompactd*  getKcompactd()  { return m_kcompactd.get(); }
    SniperKhugepaged* getKhugepaged() { return m_khugepaged.get(); }
    SniperKswapd*     getKswapd()     { return m_kswapd.get(); }
    bool isKcompactdRunning() const { return m_kcompactd && m_kcompactd->isRunning(); }

    /**
     * @brief Bind kcompactd to an allocator and start it.
     *
     * Called once the allocator has been fragmented, since kcompactd works on
     * the 2MB map that fragmentation populates.
     */
    template <typename AllocatorType>
    void startKcompactd(AllocatorType* allocator)
    {
        if (!m_kcompactd) return;
        m_kcompactd->bind(&allocator->getTwoMbMap(), allocator->getBuddyAllocator());
        m_kcompactd->start();
    }

    /**
     * @brief Get the NUMA node ID for a given physical page number.
     *
     * Delegates to the underlying physical memory allocator. NUMA-aware
     * allocators (e.g., NumaReservationTHPAllocator) return the actual node;
     * non-NUMA allocators return 0 (single implicit node).
     *
     * @param ppn Physical page number (4KB granularity)
     * @return NUMA node ID (0-based)
     */
    UInt32 getNumaNodeForPPN(UInt64 ppn) const
    {
        if (m_memory_allocator)
            return m_memory_allocator->getNumaNodeForPPN(ppn);
        return 0;
    }
    
    // ============ Swap Management ============
    
    bool isSwapEnabled() const { return m_swap_cache != nullptr; }
    SwapCache* getSwapCache() { return m_swap_cache.get(); }
    bool swapOutPage(IntPtr vpn, int app_id);
    void deletePageTableEntry(IntPtr vpn, int app_id);
    
    void setLastPageFaultCausedSwapping(bool caused_swapping) {
        m_last_pf_caused_swapping = caused_swapping;
    }
    
    // ============ HugeTLBfs Management ============
    
    /**
     * @brief Check if HugeTLBfs service is enabled
     * @return true if hugetlbfs is enabled and initialized
     */
    bool isHugeTLBfsEnabled() const { return m_hugetlbfs != nullptr && m_hugetlbfs->isEnabled(); }
    
    /**
     * @brief Get the HugeTLBfs service
     * @return Pointer to HugeTLBfs service, or nullptr if disabled
     */
    HugeTLBfs* getHugeTLBfs() { return m_hugetlbfs.get(); }
    const HugeTLBfs* getHugeTLBfs() const { return m_hugetlbfs.get(); }
    
    /**
     * @brief Allocate a huge page for an application
     * 
     * Convenience method that delegates to HugeTLBfs service.
     * 
     * @param app_id    Application requesting the page
     * @param vaddr     Virtual address to be mapped
     * @param size_2mb  True for 2MB page, false for 1GB page
     * @return Base physical page number, or -1 if allocation fails
     */
    IntPtr allocateHugePage(int app_id, IntPtr vaddr, bool size_2mb = true);
    
    /**
     * @brief Deallocate a huge page
     * 
     * @param base_ppn  Base physical page number
     * @param size_2mb  True for 2MB page, false for 1GB page
     * @return true if deallocation succeeded
     */
    bool deallocateHugePage(IntPtr base_ppn, bool size_2mb = true);
    
    // ============ Page Fault State (per-core) ============
    
    /**
     * @brief Initialize per-core page fault state vectors
     * @param num_cores Number of cores in the system
     */
    void initPerCorePageFaultState(UInt32 num_cores);
    
    PageFaultState& getPageFaultState(core_id_t core_id) {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        return m_pf_states[core_id];
    }
    const PageFaultState& getPageFaultState(core_id_t core_id) const {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        return m_pf_states[core_id];
    }
    void resetPageFaultState(core_id_t core_id) {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        m_pf_states[core_id].reset();
    }
    
    // Per-core page fault API
    bool getIsPageFault(core_id_t core_id) const {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        return m_pf_states[core_id].isActive();
    }
    void setIsPageFault(core_id_t core_id, bool is_pf) {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        if (!is_pf) m_pf_states[core_id].reset();
        else m_pf_states[core_id].is_active = true;
    }
    IntPtr getVaTriggeredPageFault(core_id_t core_id) const {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        return m_pf_states[core_id].getFaultingVA();
    }
    void setVaTriggeredPageFault(core_id_t core_id, IntPtr va) {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        m_pf_states[core_id].faulting_va = va;
    }
    void setNumRequestedFrames(core_id_t core_id, int num_frames) {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        m_pf_states[core_id].num_requested_frames = num_frames;
    }
    int getNumRequestedFrames(core_id_t core_id) const {
        assert(core_id >= 0 && (size_t)core_id < m_pf_states.size());
        return m_pf_states[core_id].getNumRequestedFrames();
    }
    
    // ============ Userspace Communication ============
    
    bool isUserspaceMimicosEnabled() const { return m_userspace_enabled; }

    /* Per-fault charge, in simulated cycles, that the MMU queues on the app
       thread when the userspace kernel handles a fault instead of the
       simulator.  The kernel sets it once its fault profile is trained, and
       resets it to 0 if the profile is abandoned, which falls back to the
       kernel-pthread-does-the-work path.  Atomic because the kernel pthread
       and the faulting thread are different host threads. */
    void setFastFaultChargeCycles(uint64_t cycles)
        { m_fast_fault_charge_cycles.store(cycles, std::memory_order_relaxed); }
    uint64_t getFastFaultChargeCycles() const
        { return m_fast_fault_charge_cycles.load(std::memory_order_relaxed); }
    
    /* Per-core event queue.
       
       This replaces a single shared MimicOSMessage slot, which could hold
       exactly one event at a time.  That is enough while only one thing ever
       posts -- a page fault, handled before the next one arrives -- but not
       once several events can be outstanding on a core at once: a new_thread
       for one worker arriving while the core is still running another, with
       a quantum_expired for the first landing on top.  With one slot the
       second event overwrites the first and it is simply lost.
       
       Each core gets its own deque, so ordering is FIFO per core and cores do
       not contend with each other, protected by its own mutex because the
       producers are different Sniper TraceThreads from the consumer. */

    /* Post an event to a core's queue. */
    template <typename... Args>
    void buildMessageWithArgs(int core_id, const std::string& message_type, Args&&... args) {
        if (core_id < 0 || (size_t)core_id >= m_event_queues.size()) core_id = 0;
        MimicOSMessage msg;
        MimicOSProtocol::buildMessage(msg, message_type, std::forward<Args>(args)...);
        UInt64 kind = msg.argv[0], arg = msg.argc > 1 ? msg.argv[1] : 0;   // before the move
        {
            std::lock_guard<std::mutex> lk(*m_event_queue_locks[core_id]);
            m_event_queues[core_id].push_back(std::move(msg));
        }
        wakeIdleWaiter(core_id);
        notifyEvent(core_id, kind, arg);
    }

    /* Same, for a poster that already holds the thread-manager lock (a
       magic handler): the wake must not take it again. */
    template <typename... Args>
    void buildMessageWithArgsLocked(int core_id, const std::string& message_type, Args&&... args) {
        if (core_id < 0 || (size_t)core_id >= m_event_queues.size()) core_id = 0;
        MimicOSMessage msg;
        MimicOSProtocol::buildMessage(msg, message_type, std::forward<Args>(args)...);
        UInt64 kind = msg.argv[0], arg = msg.argc > 1 ? msg.argv[1] : 0;   // before the move
        {
            std::lock_guard<std::mutex> lk(*m_event_queue_locks[core_id]);
            m_event_queues[core_id].push_back(std::move(msg));
        }
        wakeIdleWaiter_unlocked(core_id);
        notifyEvent(core_id, kind, arg);
    }

    /* The live feed (scripts/live.py) sees every event posted to a core's
       queue and every switch of a kernel worker to an application thread,
       through HOOK_MIMICOS_EVENT.  Kinds are the protocol codes, plus: */
    static const UInt64 EVENT_SWITCH_TO = 100;
    void notifyEvent(int core_id, UInt64 kind, UInt64 arg);

    /* Idle-worker wait.  A kernel worker whose queue is empty parks in
       SimReceiveMessage the way an idle CPU halts until an interrupt: the
       magic handler registers the worker's Sniper thread here and stalls
       it through the ThreadManager; a poster wakes it after pushing.
       register/clear are called with the thread-manager lock held (the
       handler runs under it); the wakers take that lock themselves. */
    void registerIdleWaiter(int core_id, thread_id_t thread_id);
    void clearIdleWaiter(int core_id);
    void wakeIdleWaiter(int core_id);            // takes the thread-manager lock
    void wakeIdleWaiter_unlocked(int core_id);   // caller holds it
    void wakeAllIdleWaiters();

    /* True when the core has at least one event waiting. */
    bool isEventReady(int core_id) {
        if (core_id < 0 || (size_t)core_id >= m_event_queues.size()) return false;
        std::lock_guard<std::mutex> lk(*m_event_queue_locks[core_id]);
        return !m_event_queues[core_id].empty();
    }

    /* Move the front event into `out`.  False when the queue was empty. */
    bool popEvent(int core_id, MimicOSMessage& out) {
        if (core_id < 0 || (size_t)core_id >= m_event_queues.size()) return false;
        std::lock_guard<std::mutex> lk(*m_event_queue_locks[core_id]);
        if (m_event_queues[core_id].empty()) return false;
        out = std::move(m_event_queues[core_id].front());
        m_event_queues[core_id].pop_front();
        return true;
    }

    /* Look at the front event without removing it.  The pointer is valid
       only until the next push or pop on this core. */
    MimicOSMessage* peekFront(int core_id) {
        if (core_id < 0 || (size_t)core_id >= m_event_queues.size()) core_id = 0;
        std::lock_guard<std::mutex> lk(*m_event_queue_locks[core_id]);
        if (m_event_queues[core_id].empty()) return nullptr;
        return &m_event_queues[core_id].front();
    }

    /* Number of events waiting on a core.  Exposed so a test can observe
       that queueing happened at all -- with one slot there was nothing to
       count. */
    size_t eventQueueDepth(int core_id) {
        if (core_id < 0 || (size_t)core_id >= m_event_queues.size()) return 0;
        std::lock_guard<std::mutex> lk(*m_event_queue_locks[core_id]);
        return m_event_queues[core_id].size();
    }

    bool isMessageReady(int core_id) { return isEventReady(core_id); }
    void consumeMessage(int /*core_id*/) { /* no-op: popEvent already removed it */ }
    
    // ============ Configuration ============
    
    String getName() const { return m_name; }
    SubsecondTime getPageFaultLatency() const { return m_page_fault_latency.getLatency(); }
    
    int getNumberOfPageSizes() const { return static_cast<int>(m_page_sizes.size()); }
    int* getPageSizeList() { return m_page_sizes.data(); }
    const std::vector<int>& getPageSizes() const { return m_page_sizes; }
    
    String getPageTableType() const { return m_page_table_type; }
    String getPageTableName() const { return m_page_table_name; }
    void setPageTableType(const String& type) { m_page_table_type = type; }
    void setPageTableName(const String& name) { m_page_table_name = name; }
    
    String getRangeTableType() const { return m_range_table_type; }
    String getRangeTableName() const { return m_range_table_name; }
    void setRangeTableType(const String& type) { m_range_table_type = type; }
    void setRangeTableName(const String& name) { m_range_table_name = name; }
    
    UInt64 getAccessesPerVPN(IntPtr vpn, int app_id);
    
    // ============ Protocol Codes (static, for external access) ============
    
    static std::unordered_map<std::string, uint64_t> protocol_codes_encode;
    static std::unordered_map<uint64_t, std::string> protocol_codes_decode;
    
    // ============ Per-Core Statistics ============
    
    /**
     * @brief Initialize per-core stats for a given number of cores
     * @param num_cores Number of cores in the system
     */
    void initPerCoreStats(UInt32 num_cores);
    
    /**
     * @brief Get per-core statistics (read-only)
     * @param core_id Core ID
     * @return Reference to per-core stats, or empty stats if invalid
     */
    const PerCoreStats& getPerCoreStats(core_id_t core_id) const;
    
    /**
     * @brief Get per-core statistics (mutable, for updates)
     * @param core_id Core ID
     * @return Pointer to per-core stats, or nullptr if invalid
     */
    PerCoreStats* getPerCoreStatsMutable(core_id_t core_id);
    
    /**
     * @brief Update translation statistics for a core
     * 
     * Called by MMU after each translation to update stats.
     * 
     * @param core_id       Core ID
     * @param tr_latency    Translation latency for this access
     * @param is_l2_miss    True if L2 TLB missed (triggered page walk)
     * @param walk_latency  Page walk latency (if applicable)
     */
    void updateTranslationStats(core_id_t core_id, 
                                SubsecondTime tr_latency,
                                bool is_l2_miss,
                                SubsecondTime walk_latency = SubsecondTime::Zero());
    
    /**
     * @brief Update instruction count for a core
     * @param core_id       Core ID
     * @param instructions  Number of instructions executed
     * @param cycles        Number of cycles elapsed
     */
    void updateInstructionStats(core_id_t core_id, UInt64 instructions, UInt64 cycles);
    
    /**
     * @brief Update data access statistics for a core
     * @param core_id       Core ID
     * @param latency       Data access latency
     * @param is_cache_hit  True if hit in cache (L1/L2/L3)
     */
    void updateDataAccessStats(core_id_t core_id, SubsecondTime latency, bool is_cache_hit);
    
    /**
     * @brief Check if per-core stats are initialized
     */
    bool isPerCoreStatsInitialized() const { return !m_per_core_stats.empty(); }
    
private:
    // ============ Identity ============
    String m_name;
    bool m_is_guest;
    bool m_userspace_enabled;
    
    // ============ Subsystems ============
    std::unique_ptr<PhysicalMemoryAllocator> m_memory_allocator;
    std::unique_ptr<SwapCache> m_swap_cache;

    // Linux daemon models.  Each runs from HOOK_PERIODIC at its own simulated
    // interval; absent a configuration enabling it, the pointer stays null and
    // the hook returns immediately.
    std::unique_ptr<SniperKcompactd> m_kcompactd;
    UInt64 m_kcompactd_interval_ns = 0;
    UInt64 m_kcompactd_last_scan_ns = 0;

    std::unique_ptr<SniperKhugepaged> m_khugepaged;
    UInt64 m_khugepaged_interval_ns = 0;
    UInt64 m_khugepaged_last_scan_ns = 0;

    std::unique_ptr<SniperKswapd> m_kswapd;
    UInt64 m_kswapd_interval_ns = 0;
    UInt64 m_kswapd_last_scan_ns = 0;

    static SInt64 hookKcompactdPeriodic(UInt64 self_ptr, UInt64 time_ns)
    {
        auto* self = reinterpret_cast<MimicOS*>(self_ptr);
        if (!self->m_kcompactd || self->m_kcompactd_interval_ns == 0)
            return 0;
        if (time_ns >= self->m_kcompactd_last_scan_ns + self->m_kcompactd_interval_ns) {
            self->m_kcompactd->compact_once();
            self->m_kcompactd_last_scan_ns = time_ns;
        }
        return 0;
    }

    static SInt64 hookKhugepagedPeriodic(UInt64 self_ptr, UInt64 time_ns)
    {
        auto* self = reinterpret_cast<MimicOS*>(self_ptr);
        if (!self->m_khugepaged || self->m_khugepaged_interval_ns == 0)
            return 0;
        if (time_ns >= self->m_khugepaged_last_scan_ns + self->m_khugepaged_interval_ns) {
            self->m_khugepaged->scan_once();
            self->m_khugepaged_last_scan_ns = time_ns;
        }
        return 0;
    }

    static SInt64 hookKswapdPeriodic(UInt64 self_ptr, UInt64 time_ns)
    {
        auto* self = reinterpret_cast<MimicOS*>(self_ptr);
        if (!self->m_kswapd || self->m_kswapd_interval_ns == 0)
            return 0;
        if (time_ns >= self->m_kswapd_last_scan_ns + self->m_kswapd_interval_ns) {
            self->m_kswapd->scan_once();
            self->m_kswapd_last_scan_ns = time_ns;
        }
        return 0;
    }
    std::unique_ptr<HugeTLBfs> m_hugetlbfs;
    
    // ============ Per-Application State ============
    std::unordered_map<int, std::unique_ptr<ApplicationContext>> m_applications;
    
    // ============ Page Fault State (per-core) ============
    std::vector<PageFaultState> m_pf_states;

    /* Per-core event queues, sized in initPerCorePageFaultState().  Each
       deque is guarded by its own mutex, held through unique_ptr because
       std::mutex is neither copyable nor movable and the vector needs to
       grow once at construction. */
    std::vector<std::deque<MimicOSMessage>>  m_event_queues;
    std::vector<std::unique_ptr<std::mutex>> m_event_queue_locks;
    std::vector<thread_id_t>                 m_idle_waiters;   // per core, INVALID_THREAD_ID when none
    std::atomic<uint64_t> m_fast_fault_charge_cycles{0};
    bool m_last_pf_caused_swapping;
    
    // ============ Configuration ============
    String m_page_table_type;
    String m_page_table_name;
    String m_range_table_type;
    String m_range_table_name;
    std::vector<int> m_page_sizes;
    ComponentLatency m_page_fault_latency;
    double m_target_fragmentation;
    
    // ============ Per-Core Statistics ============
    std::vector<PerCoreStats> m_per_core_stats;
    static PerCoreStats s_empty_stats;  // Returned for invalid core_id
    
    // ============ Logging ============
    std::string m_log_file_name;
    std::ofstream m_log;
};

#endif // MIMICOS_H
