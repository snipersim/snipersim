/**
 * @file kernel_arena.h
 * @brief Slab arena for MimicOS internal metadata.
 *
 * Today, MimicOS allocates its own metadata (page-table frames, free-list
 * nodes, fault-cache entries, Task objects, ...) via host `new` / `malloc`.
 * Those allocations consume zero simulated DRAM and are invisible to
 * Sniper's MMU + cache models, which makes any study of "MimicOS kernel
 * footprint" unrealistic.
 *
 * KernelArena gives MimicOS a real, accountable backing store:
 *   1. mmap a chunk of host memory equal to the simulated kernel reserve;
 *      MimicOS-internal objects are placement-new'd into this region so
 *      every load/store hits real host VAs (which Sniper translates and
 *      simulates like any other memory access).
 *   2. Each allocation is also charged against the simulated kernel
 *      reserve (PFNs are consumed via `handle_page_table_allocations`),
 *      so the simulated MMU + accounting layers see the footprint grow.
 *   3. Per-type byte counters expose where the kernel memory goes.
 *
 * Slab discipline: single bump allocator per category (PAGE_TABLE,
 * FAULT_CACHE, TASK, SHARED_REGION, GENERIC).  The arena does not free
 * (matches Linux kernel slab behaviour for these long-lived objects).
 */
#pragma once

#include <cstddef>   // std::max_align_t, used by alloc() below
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <atomic>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

class PhysicalMemoryAllocator;

/* Categories of internal MimicOS metadata.  Add new ones as we wire more
   call sites through the arena. */
enum class KArenaSlab : uint8_t {
    PAGE_TABLE     = 0,   /* radix PT frames, indirect entries          */
    FAULT_CACHE    = 1,   /* FaultCache entries                         */
    TASK           = 2,   /* Scheduler Task objects                     */
    SHARED_REGION  = 3,   /* SharedRegion descriptors                   */
    GENERIC        = 4,   /* anything else, catch-all                   */
    NUM_SLABS
};

inline const char* karena_slab_name(KArenaSlab s) {
    static const char* names[] = {
        "page_table", "fault_cache", "task", "shared_region", "generic"
    };
    int i = static_cast<int>(s);
    return (i >= 0 && i < (int)KArenaSlab::NUM_SLABS) ? names[i] : "unknown";
}

class KernelArena {
public:
    KernelArena()
        : enabled_(false), base_(nullptr), size_(0), used_(0),
          allocator_(nullptr), pfns_consumed_(0) {
        for (int i = 0; i < (int)KArenaSlab::NUM_SLABS; i++) {
            slab_bytes_[i] = 0;
            slab_count_[i] = 0;
        }
    }

    ~KernelArena() {
        if (base_ && base_ != MAP_FAILED)
            munmap(base_, size_);
    }

    /**
     * Initialise.  Should be called once after the physical memory
     * allocator is constructed (so we can charge against simulated kernel
     * reserve via handle_page_table_allocations).
     *
     * @param kernel_reserve_bytes  bytes of simulated kernel memory we may
     *                              consume (typically a fraction of the
     *                              [pmem_alloc] kernel_size INI value).
     * @param phys_alloc            backend allocator used to charge PFNs
     */
    bool init(size_t kernel_reserve_bytes,
              PhysicalMemoryAllocator* phys_alloc) {
        if (enabled_) return true;

        /* Round up to page boundary */
        size_t pg = sysconf(_SC_PAGESIZE);
        size_ = (kernel_reserve_bytes + pg - 1) & ~(pg - 1);

        base_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base_ == MAP_FAILED) {
            std::cerr << "[KernelArena] mmap " << size_ << " failed: "
                      << strerror(errno) << std::endl;
            base_ = nullptr;
            return false;
        }

        allocator_ = phys_alloc;
        used_ = 0;
        enabled_ = true;

        std::cout << "[KernelArena] Initialised: " << size_ << " bytes ("
                  << (size_ / (1024 * 1024)) << " MB) reserved at host VA "
                  << base_ << std::endl;
        return true;
    }

    /**
     * Typed allocation: placement-new T(args...) into the arena.
     * The returned pointer is in host memory (so the C++ object is real),
     * but each underlying 4 KB page is also charged against the simulated
     * kernel reserve so Sniper's MMU/footprint counters see the cost.
     */
    template <typename T, typename... Args>
    T* alloc(KArenaSlab slab, Args&&... args) {
        void* p = raw_alloc(sizeof(T), alignof(T), slab);
        if (!p) return nullptr;
        return new (p) T(std::forward<Args>(args)...);
    }

    /**
     * Raw byte allocation.  Returns nullptr on OOM (arena exhausted).
     * Each new 4 KB page boundary triggers a charge against the simulated
     * kernel reserve.
     */
    void* raw_alloc(size_t bytes, size_t align, KArenaSlab slab) {
        if (!enabled_) {
            /* Fallback: host alloc, count as generic so users still
               see the footprint number even if the arena isn't on. */
            void* p = std::aligned_alloc(align ? align : 16,
                                         (bytes + 15) & ~size_t(15));
            if (p) {
                slab_bytes_[(int)slab] += bytes;
                slab_count_[(int)slab]++;
            }
            return p;
        }

        /* Align used_ */
        size_t a = align ? align : alignof(std::max_align_t);
        size_t aligned = (used_ + a - 1) & ~(a - 1);
        if (aligned + bytes > size_) {
            /* Arena exhausted */
            std::cerr << "[KernelArena] OUT OF MEMORY: requested " << bytes
                      << " for slab " << karena_slab_name(slab)
                      << ", used=" << used_ << " of " << size_ << std::endl;
            return nullptr;
        }

        /* Charge any new 4 KB pages we crossed against the simulated
           kernel reserve.  This is the "footprint shows up in simulated
           memory" piece. */
        size_t before_page = used_ / 4096;
        size_t after_page  = (aligned + bytes + 4095) / 4096;
        for (size_t i = before_page; i < after_page; i++) {
            charge_pfn();
        }

        void* p = static_cast<char*>(base_) + aligned;
        used_ = aligned + bytes;
        slab_bytes_[(int)slab] += bytes;
        slab_count_[(int)slab]++;
        return p;
    }

    /* ---- Stats ---- */
    bool   enabled() const     { return enabled_; }
    size_t total_size() const  { return size_; }
    size_t bytes_used() const  { return used_; }
    size_t pfns_consumed() const { return pfns_consumed_; }
    uint64_t slab_bytes(KArenaSlab s) const { return slab_bytes_[(int)s]; }
    uint64_t slab_count(KArenaSlab s) const { return slab_count_[(int)s]; }

    void dump_csv(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return;
        f << "metric,value\n";
        f << "enabled," << (enabled_ ? 1 : 0) << "\n";
        f << "arena_total_bytes," << size_ << "\n";
        f << "arena_used_bytes," << used_ << "\n";
        f << "arena_used_pct," << (size_ ? (100.0 * used_ / size_) : 0) << "\n";
        f << "simulated_pfns_consumed," << pfns_consumed_ << "\n";
        f << "\nslab,bytes,count\n";
        for (int i = 0; i < (int)KArenaSlab::NUM_SLABS; i++) {
            f << karena_slab_name((KArenaSlab)i) << ","
              << slab_bytes_[i] << "," << slab_count_[i] << "\n";
        }
    }

private:
    /* Charge a single 4 KB page against the simulated kernel reserve.
       Defined in mimicos.cc to avoid the include cycle (we'd need the
       full PhysicalMemoryAllocator definition here). */
    void charge_pfn();

    bool   enabled_;
    void*  base_;
    size_t size_;
    size_t used_;
    PhysicalMemoryAllocator* allocator_;
    uint64_t pfns_consumed_;
    uint64_t slab_bytes_[(int)KArenaSlab::NUM_SLABS];
    uint64_t slab_count_[(int)KArenaSlab::NUM_SLABS];
};
