/**
 * @file task.h
 * @brief Lightweight task abstraction for MimicOS scheduling.
 *
 * A Task represents a schedulable execution entity (analogous to a
 * Linux task_struct but dramatically simplified).  It maintains just
 * enough state to model VM/scheduling interaction studies.
 */
#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <vector>

/* ------------------------------------------------------------------ */
/*  Task state machine                                                 */
/* ------------------------------------------------------------------ */

enum class TaskState : uint8_t {
    CREATED    = 0,   /* task exists but not yet runnable */
    RUNNABLE   = 1,   /* on a runqueue, eligible to run */
    RUNNING    = 2,   /* currently executing on a core */
    BLOCKED    = 3,   /* waiting for event (fault, I/O, shm) */
    DEAD       = 4,   /* terminated */
};

inline const char* task_state_name(TaskState s) {
    static const char* names[] = {
        "created", "runnable", "running", "blocked", "dead"
    };
    return names[static_cast<int>(s)];
}

/* ------------------------------------------------------------------ */
/*  Wait channel (reason for blocking)                                 */
/* ------------------------------------------------------------------ */

enum class WaitChannel : uint8_t {
    NONE             = 0,
    PAGE_FAULT       = 1,  /* waiting for page fault resolution */
    SHARED_MEM_WAIT  = 2,  /* waiting on shared-memory notify */
    QUEUE_WAIT       = 3,  /* waiting on a queue primitive */
    YIELD            = 4,  /* voluntary yield */
    TIMER            = 5,  /* sleeping */
};

/* ------------------------------------------------------------------ */
/*  Scheduling entity (per-task scheduler metadata)                    */
/* ------------------------------------------------------------------ */

struct SchedEntity {
    uint64_t vruntime        = 0;  /* virtual runtime (CFS-like) */
    uint64_t runtime_ns      = 0;  /* actual runtime in ns */
    uint32_t priority         = 120; /* nice-level mapped priority (default=120) */
    uint32_t weight           = 1024; /* weight derived from priority */
    uint32_t timeslice_ns     = 4000000; /* 4ms default timeslice */
    uint32_t timeslice_remaining_ns = 0;

    /* VM-aware scheduling hints */
    uint64_t recent_fault_count = 0;
    uint64_t recent_tlb_misses  = 0;
    uint8_t  preferred_numa_node = 0;
    double   page_locality_score = 1.0; /* 1.0 = perfect locality */
};

/* ------------------------------------------------------------------ */
/*  Task                                                               */
/* ------------------------------------------------------------------ */

struct Task {
    /* Identity */
    int32_t   task_id;
    std::string name;

    /* Stage 5 (Apr 19 2026): for live-spawn tasks, the opaque
       app_thread_id (== encode(app_id, thread_num)) that
       SimContextSwitchTo consumes.  Zero for trace-based / legacy tasks
       that aren't driven by the kernel-pthread dispatch loop. */
    uint64_t  app_thread_id = 0;

    /* State */
    TaskState   state         = TaskState::CREATED;
    WaitChannel wait_channel  = WaitChannel::NONE;

    /* Core assignment */
    int32_t   current_core    = -1;  /* core currently running on (-1 = none) */
    uint64_t  affinity_mask   = ~0ULL; /* bitmask of allowed cores */

    /* Address space (pointer to page table, VMA list, etc.) */
    void*     mm_ptr          = nullptr;  /* opaque pointer to mm_struct equivalent */

    /* Scheduling entity */
    SchedEntity sched;

    /* Statistics */
    uint64_t  context_switches   = 0;
    uint64_t  voluntary_switches = 0;
    uint64_t  involuntary_switches = 0;
    uint64_t  total_fault_count  = 0;
    uint64_t  total_fault_cost_ns = 0;
    uint64_t  total_blocked_ns   = 0;
    uint64_t  total_runtime_ns   = 0;

    /* Shared-memory wait channel data */
    uint32_t  shm_wait_region_id = 0;

    /* ---- Methods ---- */

    bool is_runnable() const {
        return state == TaskState::RUNNABLE || state == TaskState::RUNNING;
    }

    bool can_run_on(int32_t core) const {
        if (core < 0 || core >= 64) return false;
        return (affinity_mask & (1ULL << core)) != 0;
    }

    void set_running(int32_t core) {
        state = TaskState::RUNNING;
        current_core = core;
    }

    void set_runnable() {
        state = TaskState::RUNNABLE;
        current_core = -1;
    }

    void set_blocked(WaitChannel wc) {
        state = TaskState::BLOCKED;
        wait_channel = wc;
        current_core = -1;
    }

    void set_dead() {
        state = TaskState::DEAD;
        current_core = -1;
    }

    /**
     * Charge fault cost against the task's runtime and update stats.
     */
    void charge_fault(uint64_t cost_ns) {
        total_fault_count++;
        total_fault_cost_ns += cost_ns;
        sched.runtime_ns += cost_ns;
        sched.vruntime += cost_ns / sched.weight; /* weighted vruntime */
        sched.recent_fault_count++;
    }
};
