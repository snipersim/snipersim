/**
 * @file shared_region.h
 * @brief Shared-memory region manager for MimicOS.
 *
 * Allows MimicOS and userspace worker threads to communicate via shared
 * memory with well-defined visibility, ownership, and timing semantics.
 *
 * Stage 1: Host-side practical shared memory (memfd_create / POSIX shm).
 * Stage 2: Simulated shared-memory semantics (accesses charged to memory
 *           hierarchy) — controlled by visibility mode.
 *
 * Three visibility modes:
 *   1. HOST_COHERENT_ONLY:   for control-plane communication
 *   2. SIMULATOR_ACCOUNTED:  accesses also charged to simulated hierarchy
 *   3. RING_BUFFER:          SPSC/MPMC ring for low-overhead messaging
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

/* ------------------------------------------------------------------ */
/*  Shared Region types                                                */
/* ------------------------------------------------------------------ */

enum class SharedVisibility : uint8_t {
    HOST_COHERENT_ONLY   = 0,  /* functional only, no simulator charging */
    SIMULATOR_ACCOUNTED  = 1,  /* accesses also charged to memory model */
    RING_BUFFER          = 2,  /* producer/consumer ring buffer mode */
};

enum class SharedPerm : uint8_t {
    READ_ONLY   = 0,
    READ_WRITE  = 1,
};

using SharedRegionId = uint32_t;

/* ------------------------------------------------------------------ */
/*  Shared Region descriptor                                           */
/* ------------------------------------------------------------------ */

struct SharedRegion {
    SharedRegionId  id;
    std::string     name;           /* human-readable label */
    void*           host_base;      /* host virtual address (mmap'd) */
    size_t          size;           /* region size in bytes */
    int             backing_fd;     /* memfd or shm fd */
    SharedPerm      perm;
    SharedVisibility visibility;

    /* Simulated mapping metadata (Stage 2) */
    uint64_t        sim_va_base;    /* base VA in simulated address space */
    uint8_t         numa_node;      /* preferred NUMA placement */
    bool            pinned;         /* host pages pinned (mlock)? */
    bool            allow_thp;      /* allow THP for this region? */

    /* Ownership */
    int             owner_task_id;  /* creating task (-1 = kernel) */
    int             ref_count;      /* tasks with active mappings */
};

/* ------------------------------------------------------------------ */
/*  Lock-free SPSC ring buffer                                         */
/* ------------------------------------------------------------------ */

/**
 * Single-Producer, Single-Consumer ring buffer in shared memory.
 * The ring header is placed at the start of the shared region.
 * Data follows immediately after.
 */
struct alignas(64) SPSCRingHeader {
    std::atomic<uint64_t> write_idx;   /* producer increments */
    char _pad1[56];                     /* avoid false sharing */
    std::atomic<uint64_t> read_idx;    /* consumer increments */
    char _pad2[56];
    uint64_t capacity;                  /* number of slots (power of 2) */
    uint64_t slot_size;                 /* bytes per slot */
};

class SPSCRingBuffer {
public:
    SPSCRingBuffer() : hdr_(nullptr), data_(nullptr) {}

    /**
     * Initialize over an existing shared region.
     * The region must be large enough for header + capacity * slot_size.
     */
    bool init(void* region_base, size_t region_size,
              uint64_t capacity, uint64_t slot_size, bool is_producer) {
        if (region_size < sizeof(SPSCRingHeader) + capacity * slot_size)
            return false;

        hdr_ = reinterpret_cast<SPSCRingHeader*>(region_base);
        data_ = reinterpret_cast<char*>(region_base) + sizeof(SPSCRingHeader);

        if (is_producer) {
            /* Producer initialises the header */
            hdr_->write_idx.store(0, std::memory_order_relaxed);
            hdr_->read_idx.store(0, std::memory_order_relaxed);
            hdr_->capacity = capacity;
            hdr_->slot_size = slot_size;
        }
        /* Consumer reads existing header */

        return true;
    }

    /**
     * Try to enqueue a message. Returns false if ring is full.
     */
    bool try_push(const void* data, size_t len) {
        if (!hdr_ || len > hdr_->slot_size) return false;

        uint64_t wr = hdr_->write_idx.load(std::memory_order_relaxed);
        uint64_t rd = hdr_->read_idx.load(std::memory_order_acquire);

        if (wr - rd >= hdr_->capacity)
            return false;  /* full */

        uint64_t slot = wr & (hdr_->capacity - 1);
        memcpy(data_ + slot * hdr_->slot_size, data, len);

        hdr_->write_idx.store(wr + 1, std::memory_order_release);
        return true;
    }

    /**
     * Try to dequeue a message. Returns false if ring is empty.
     */
    bool try_pop(void* data, size_t max_len) {
        if (!hdr_) return false;

        uint64_t rd = hdr_->read_idx.load(std::memory_order_relaxed);
        uint64_t wr = hdr_->write_idx.load(std::memory_order_acquire);

        if (rd >= wr)
            return false;  /* empty */

        uint64_t slot = rd & (hdr_->capacity - 1);
        size_t copy_len = std::min(max_len, (size_t)hdr_->slot_size);
        memcpy(data, data_ + slot * hdr_->slot_size, copy_len);

        hdr_->read_idx.store(rd + 1, std::memory_order_release);
        return true;
    }

    uint64_t pending() const {
        if (!hdr_) return 0;
        uint64_t wr = hdr_->write_idx.load(std::memory_order_acquire);
        uint64_t rd = hdr_->read_idx.load(std::memory_order_acquire);
        return wr - rd;
    }

    bool is_empty() const { return pending() == 0; }
    bool is_full() const {
        return hdr_ && pending() >= hdr_->capacity;
    }

private:
    SPSCRingHeader* hdr_;
    char* data_;
};

/* ------------------------------------------------------------------ */
/*  Shared Region Manager                                              */
/* ------------------------------------------------------------------ */

class SharedRegionManager {
public:
    SharedRegionManager() : next_id_(1) {}

    ~SharedRegionManager() {
        for (auto& [id, region] : regions_) {
            if (region.host_base && region.host_base != MAP_FAILED) {
                munmap(region.host_base, region.size);
            }
            if (region.backing_fd >= 0) {
                close(region.backing_fd);
            }
        }
    }

    /**
     * Create and register a new shared region backed by memfd_create.
     *
     * Returns the region ID, or 0 on failure.
     */
    SharedRegionId register_shared_region(
        const std::string& name,
        size_t size,
        SharedPerm perm = SharedPerm::READ_WRITE,
        SharedVisibility visibility = SharedVisibility::HOST_COHERENT_ONLY,
        int owner_task_id = -1,
        uint8_t numa_node = 0,
        bool pinned = false)
    {
        /* Create anonymous fd */
        int fd = memfd_create(name.c_str(), MFD_CLOEXEC);
        if (fd < 0) {
            std::cerr << "[SharedRegionManager] memfd_create failed: "
                      << strerror(errno) << std::endl;
            return 0;
        }

        if (ftruncate(fd, size) < 0) {
            std::cerr << "[SharedRegionManager] ftruncate failed" << std::endl;
            close(fd);
            return 0;
        }

        int mmap_prot = PROT_READ;
        if (perm == SharedPerm::READ_WRITE) mmap_prot |= PROT_WRITE;

        void* base = mmap(nullptr, size, mmap_prot, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            std::cerr << "[SharedRegionManager] mmap failed" << std::endl;
            close(fd);
            return 0;
        }

        if (pinned) {
            mlock(base, size);
        }

        SharedRegionId id = next_id_++;
        SharedRegion region;
        region.id = id;
        region.name = name;
        region.host_base = base;
        region.size = size;
        region.backing_fd = fd;
        region.perm = perm;
        region.visibility = visibility;
        region.sim_va_base = 0;
        region.numa_node = numa_node;
        region.pinned = pinned;
        region.allow_thp = false;
        region.owner_task_id = owner_task_id;
        region.ref_count = 1;

        regions_[id] = region;

        std::cout << "[SharedRegionManager] Created region '" << name
                  << "' id=" << id << " size=" << size
                  << " vis=" << static_cast<int>(visibility) << std::endl;

        return id;
    }

    /**
     * Map an existing shared region into a task's address space.
     * Bumps ref_count. Returns host pointer or nullptr.
     */
    void* map_shared_region(SharedRegionId id, int task_id,
                            uint64_t sim_va = 0,
                            SharedPerm perm = SharedPerm::READ_WRITE) {
        auto it = regions_.find(id);
        if (it == regions_.end()) return nullptr;

        SharedRegion& r = it->second;
        r.ref_count++;
        if (sim_va != 0) r.sim_va_base = sim_va;

        (void)task_id;  /* bookkeeping for future per-task tracking */
        (void)perm;

        return r.host_base;
    }

    /**
     * Unmap a shared region from a task. Decrements ref_count.
     * Region is destroyed when ref_count reaches 0.
     */
    void unmap_shared_region(SharedRegionId id, int task_id) {
        auto it = regions_.find(id);
        if (it == regions_.end()) return;

        SharedRegion& r = it->second;
        r.ref_count--;
        (void)task_id;

        if (r.ref_count <= 0) {
            if (r.host_base && r.host_base != MAP_FAILED)
                munmap(r.host_base, r.size);
            if (r.backing_fd >= 0)
                close(r.backing_fd);
            regions_.erase(it);
        }
    }

    /**
     * Get the host-side pointer for a shared region.
     */
    void* get_host_ptr(SharedRegionId id) const {
        auto it = regions_.find(id);
        if (it == regions_.end()) return nullptr;
        return it->second.host_base;
    }

    /**
     * Get the fd for a shared region (for mapping in other processes).
     */
    int get_fd(SharedRegionId id) const {
        auto it = regions_.find(id);
        if (it == regions_.end()) return -1;
        return it->second.backing_fd;
    }

    /**
     * Get region info.
     */
    const SharedRegion* get_region(SharedRegionId id) const {
        auto it = regions_.find(id);
        if (it == regions_.end()) return nullptr;
        return &it->second;
    }

    size_t region_count() const { return regions_.size(); }

    /**
     * Full-fence barrier across all regions.
     */
    void barrier() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /**
     * Notify: signal waiting threads (lightweight, uses atomic flag).
     * For more sophisticated wakeup, use futex or condition variable.
     */
    void notify(SharedRegionId id) {
        auto it = regions_.find(id);
        if (it == regions_.end()) return;
        /* Simple atomic store as notification flag at region[0] */
        if (it->second.host_base) {
            auto* flag = reinterpret_cast<std::atomic<uint32_t>*>(it->second.host_base);
            flag->store(1, std::memory_order_release);
        }
    }

private:
    SharedRegionId next_id_;
    std::unordered_map<SharedRegionId, SharedRegion> regions_;
};
