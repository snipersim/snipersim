/**
 * @file scheduler.h
 * @brief MimicOS scheduler — policies and dispatch.
 *
 * Supports four scheduling policies implemented in order of complexity:
 *   1. sched_fifo_simple  — cooperative FIFO
 *   2. sched_rr_simple    — round-robin with configurable timeslice
 *   3. sched_cfs_lite     — CFS-like weighted fair scheduling
 *   4. sched_vm_aware     — VM-aware (NUMA, TLB, fault-rate hints)
 *
 * The scheduler is OPTIONAL — it can be disabled for backward-
 * compatible baseline runs via configuration.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <deque>

#include "mm/scheduler/task.h"
#include "mm/scheduler/runqueue.h"
#include "mm/kernel_arena.h"

/* ------------------------------------------------------------------ */
/*  Scheduling policy enum                                             */
/* ------------------------------------------------------------------ */

enum class SchedPolicy : uint8_t {
    DISABLED      = 0,  /* no scheduling, single-task mode */
    FIFO_SIMPLE   = 1,
    RR_SIMPLE     = 2,
    CFS_LITE      = 3,
    VM_AWARE      = 4,
};

inline const char* sched_policy_name(SchedPolicy p) {
    static const char* names[] = {
        "disabled", "fifo_simple", "rr_simple", "cfs_lite", "vm_aware"
    };
    return names[static_cast<int>(p)];
}

/* ------------------------------------------------------------------ */
/*  Timer event (for preemptive scheduling)                            */
/* ------------------------------------------------------------------ */

struct TimerEvent {
    uint64_t fire_time_ns;     /* absolute time when event fires */
    int32_t  task_id;          /* task to wake or preempt */
    enum Type { TIMESLICE_EXPIRED, WAKEUP_TIMER } type;
};

/* ------------------------------------------------------------------ */
/*  Scheduler configuration                                            */
/* ------------------------------------------------------------------ */

struct SchedulerConfig {
    SchedPolicy policy = SchedPolicy::DISABLED;
    int num_cores = 1;

    /* Timeslice for RR/CFS (ns) */
    uint64_t default_timeslice_ns = 4000000;  /* 4ms */

    /* CFS parameters */
    uint64_t min_granularity_ns = 1000000;    /* 1ms min scheduling quantum */
    uint64_t target_latency_ns  = 20000000;   /* 20ms target scheduling period */

    /* VM-aware policy parameters */
    bool prefer_numa_local  = true;
    bool avoid_fault_heavy  = true;
    double fault_rate_threshold = 0.1; /* fault/instruction ratio threshold */

    /* Cooperative yield points */
    bool yield_after_fault        = true;
    bool yield_after_queue_wait   = true;
    bool yield_after_shm_notify   = true;
    uint64_t yield_instruction_quantum = 0; /* 0 = no instruction-based yield */
};

/* ------------------------------------------------------------------ */
/*  Scheduler                                                          */
/* ------------------------------------------------------------------ */

class Scheduler {
public:
    Scheduler() : enabled_(false), arena_(nullptr) {}

    /* Optional: route Task allocations through a KernelArena so each Task
       is accounted in the simulated kernel reserve.  When unset, Tasks
       use the host heap (legacy behaviour). */
    void set_arena(KernelArena* arena) { arena_ = arena; }

    void configure(const SchedulerConfig& cfg) {
        config_ = cfg;
        enabled_ = (cfg.policy != SchedPolicy::DISABLED);

        if (enabled_) {
            runqueues_.clear();
            for (int i = 0; i < cfg.num_cores; i++) {
                runqueues_.emplace_back(i);
            }
            std::cout << "[MimicOS Scheduler] Configured: policy="
                      << sched_policy_name(cfg.policy)
                      << " cores=" << cfg.num_cores << std::endl;
        }
    }

    bool is_enabled() const { return enabled_; }
    SchedPolicy policy() const { return config_.policy; }

    /* ---- Task lifecycle ---- */

    /**
     * Create and register a new task.  Returns task pointer.
     */
    Task* create_task(const std::string& name, int32_t preferred_core = 0) {
        std::lock_guard<std::mutex> lk(tasks_lock_);
        return create_task_unlocked(name, preferred_core);
    }

    /**
     * Make a task runnable and enqueue it.
     */
    void wake_up(Task* t, int32_t target_core = -1) {
        if (!enabled_ || !t) return;
        int32_t core = select_core(t, target_core);   /* lockless load read */
        if (core < 0 || core >= (int32_t)runqueues_.size()) core = 0;
        RunQueue& rq = runqueues_[core];
        std::lock_guard<std::mutex> lk(rq.lock);      /* only the target core's lock */
        if (t->state == TaskState::RUNNING) return;
        t->set_runnable();
        rq.enqueue(t);
        stats_.wakeups++;
        if (config_.policy >= SchedPolicy::CFS_LITE) {
            Task* cur = rq.get_current();
            if (cur && should_preempt(cur, t)) preempt_unlocked(core);
        }
    }

    /**
     * Block a running task on a wait channel.
     */
    void block(Task* t, WaitChannel wc) {
        if (!enabled_ || !t) return;
        int32_t core = -1;
        std::unique_lock<std::mutex> rl = lock_task_rq(t, core);
        if (core >= 0) runqueues_[core].dequeue(t);
        t->set_blocked(wc);
        {
            std::lock_guard<std::mutex> wl(wait_lock_);   /* rq -> wait, never reverse */
            wait_queue_.enqueue(t);
        }
        stats_.blocks++;
        if (core >= 0) run_schedule_unlocked(core);
    }

    /**
     * Wake tasks blocked on a specific channel.
     */
    int wake_blocked(WaitChannel wc) {
        /* Drain under wait_lock_, wake after releasing it: wake_up takes an
           rq lock, and block() takes wait_lock_ while holding one, so waking
           inline here would invert that order. */
        std::vector<Task*> woken;
        {
            std::lock_guard<std::mutex> wl(wait_lock_);
            wait_queue_.wake_all(wc, [&](Task* t) { woken.push_back(t); });
        }
        for (Task* t : woken) wake_up(t);
        return (int)woken.size();
    }

    /**
     * Voluntary yield (cooperative scheduling).
     */
    void yield(Task* t) {
        if (!enabled_ || !t) return;
        int32_t core = -1;
        std::unique_lock<std::mutex> rl = lock_task_rq(t, core);
        yield_unlocked(t, core);
    }

    /* ---- Scheduling decisions ---- */

    /**
     * Run the scheduling algorithm on a core and dispatch the chosen task.
     */
    void schedule(int32_t core) {
        if (!enabled_) return;
        if (core < 0 || core >= (int32_t)runqueues_.size()) return;
        std::lock_guard<std::mutex> lk(runqueues_[core].lock);
        run_schedule_unlocked(core);
    }

    /**
     * Timeslice tick — called periodically to check for preemption.
     */
    void tick(int32_t core, uint64_t elapsed_ns) {
        if (!enabled_) return;
        if (config_.policy < SchedPolicy::RR_SIMPLE) return;
        if (core < 0 || core >= (int32_t)runqueues_.size()) return;
        RunQueue& rq = runqueues_[core];
        std::lock_guard<std::mutex> lk(rq.lock);
        Task* cur = rq.get_current();
        if (!cur) return;
        cur->sched.runtime_ns += elapsed_ns;
        cur->total_runtime_ns += elapsed_ns;
        cur->sched.vruntime += elapsed_ns / cur->sched.weight;
        if (config_.policy == SchedPolicy::RR_SIMPLE) {
            if (cur->sched.timeslice_remaining_ns <= elapsed_ns) {
                cur->involuntary_switches++;
                rq.rotate();
                run_schedule_unlocked(core);
                rq.stats().preemptions++;
                stats_.preemptions++;
            } else {
                cur->sched.timeslice_remaining_ns -= elapsed_ns;
            }
        } else if (config_.policy >= SchedPolicy::CFS_LITE) {
            /* preempt if another task's vruntime is behind ours by more than
               min_granularity */
            Task* candidate = rq.pick_next_min_vruntime();
            if (candidate && candidate != cur &&
                cur->sched.vruntime > candidate->sched.vruntime + config_.min_granularity_ns) {
                cur->involuntary_switches++;
                run_schedule_unlocked(core);
                rq.stats().preemptions++;
                stats_.preemptions++;
            }
        }
    }

    /**
     * Notify the scheduler that a task took a page fault.
     * Charges the fault cost and optionally yields.
     */
    void on_page_fault(Task* t, uint64_t fault_cost_ns) {
        if (!enabled_ || !t) return;
        int32_t core = -1;
        std::unique_lock<std::mutex> rl = lock_task_rq(t, core);
        t->charge_fault(fault_cost_ns);
        if (config_.yield_after_fault && core >= 0) {
            if (config_.policy == SchedPolicy::FIFO_SIMPLE) yield_unlocked(t, core);
        }
    }

    /* ---- Queries ---- */

    Task* get_task(int32_t task_id) const {
        std::lock_guard<std::mutex> lk(tasks_lock_);
        auto it = task_map_.find(task_id);
        return (it != task_map_.end()) ? it->second : nullptr;
    }

    size_t task_count() const {
        std::lock_guard<std::mutex> lk(tasks_lock_);
        return tasks_.size() + arena_owned_tasks_.size();
    }
    /* The task a core is running, read under that core's lock.  Replaces
       get_runqueue(core).get_current(), which read it under no lock. */
    Task* current_task(int32_t core) const {
        if (core < 0 || core >= (int32_t)runqueues_.size()) return nullptr;
        std::lock_guard<std::mutex> lk(runqueues_[core].lock);
        return runqueues_[core].get_current();
    }

    const RunQueue& get_runqueue(int32_t core) const {
        return runqueues_[core];
    }

    /* ---- Statistics ---- */

    struct Stats {   /* atomic: incremented from any core without a shared lock */
        std::atomic<uint64_t> context_switches{0};
        std::atomic<uint64_t> preemptions{0};
        std::atomic<uint64_t> wakeups{0};
        std::atomic<uint64_t> blocks{0};
    };

    const Stats& stats() const { return stats_; }

    void dump_stats(const std::string& filepath) const {
        std::ofstream f(filepath);
        if (!f.is_open()) return;
        f << "metric,value\n";
        f << "policy," << sched_policy_name(config_.policy) << "\n";
        f << "num_cores," << config_.num_cores << "\n";
        {
            std::lock_guard<std::mutex> lk(tasks_lock_);
            f << "num_tasks," << (tasks_.size() + arena_owned_tasks_.size()) << "\n";
            f << "tasks_in_arena," << arena_owned_tasks_.size() << "\n";
            f << "tasks_on_heap," << tasks_.size() << "\n";
        }
        f << "context_switches," << stats_.context_switches << "\n";
        f << "preemptions," << stats_.preemptions << "\n";
        f << "wakeups," << stats_.wakeups << "\n";
        f << "blocks," << stats_.blocks << "\n";
        f << "\ntask_id,name,state,total_faults,fault_cost_ns,"
             "runtime_ns,voluntary_csw,involuntary_csw\n";
        auto dump_task = [&](Task* t) {
            f << t->task_id << "," << t->name << ","
              << task_state_name(t->state) << ","
              << t->total_fault_count << "," << t->total_fault_cost_ns << ","
              << t->total_runtime_ns << "," << t->voluntary_switches << ","
              << t->involuntary_switches << "\n";
        };
        {
            std::lock_guard<std::mutex> lk(tasks_lock_);
            for (const auto& t : tasks_) dump_task(t.get());
            for (Task* t : arena_owned_tasks_) dump_task(t);
        }
        f << "\ncore,rq_length,csw,preemptions\n";
        for (size_t i = 0; i < runqueues_.size(); i++) {
            std::lock_guard<std::mutex> lk(runqueues_[i].lock);
            f << i << "," << runqueues_[i].length() << ","
              << runqueues_[i].stats().context_switches << ","
              << runqueues_[i].stats().preemptions << "\n";
        }
    }

private:
    /* ---- VM-aware scheduling ---- */

    Task* pick_vm_aware(RunQueue& rq) {
        if (rq.empty()) return nullptr;

        /* Score each task: lower = better (picked first) */
        Task* best = nullptr;
        double best_score = 1e18;

        for (Task* t : rq.tasks()) {
            if (t->state != TaskState::RUNNABLE) continue;

            double score = static_cast<double>(t->sched.vruntime);

            /* Prefer tasks whose working set is local */
            if (config_.prefer_numa_local) {
                if (t->sched.preferred_numa_node != 0) {
                    score *= 1.1;  /* penalty for non-local */
                }
                score /= (t->sched.page_locality_score + 0.01);
            }

            /* Avoid running multiple fault-heavy tasks on same core */
            if (config_.avoid_fault_heavy) {
                double fault_rate = (t->total_runtime_ns > 0)
                    ? (double)t->sched.recent_fault_count / (t->total_runtime_ns / 1e6)
                    : 0;
                if (fault_rate > config_.fault_rate_threshold) {
                    score *= 1.2;  /* mild penalty */
                }
            }

            if (score < best_score) {
                best_score = score;
                best = t;
            }
        }

        return best;
    }

    /**
     * Select the best core for a task.
     */
    int32_t select_core(Task* t, int32_t hint) {
        if (hint >= 0 && hint < (int32_t)runqueues_.size() && t->can_run_on(hint))
            return hint;
        /* Lockless: compares each core's atomic nr_running, as Linux does
           when picking a CPU, tolerating a momentarily stale count. */
        int32_t best = -1;
        uint32_t min_load = UINT32_MAX;
        for (size_t i = 0; i < runqueues_.size(); i++) {
            uint32_t n = runqueues_[i].nr_running();
            if (t->can_run_on(i) && n < min_load) { min_load = n; best = (int32_t)i; }
        }
        return best >= 0 ? best : 0;
    }

    /**
     * Check if new_task should preempt current_task.
     */
    bool should_preempt(Task* current, Task* incoming) {
        if (!current || !incoming) return false;
        /* Preempt if incoming has significantly less vruntime */
        return incoming->sched.vruntime + config_.min_granularity_ns
               < current->sched.vruntime;
    }

    /**
     * Preempt the current task on a core.
     */
    /* Caller holds runqueues_[core].lock. */
    void preempt_unlocked(int32_t core) {
        if (core < 0 || core >= (int32_t)runqueues_.size()) return;
        RunQueue& rq = runqueues_[core];
        Task* cur = rq.get_current();
        if (cur) {
            cur->involuntary_switches++;
            cur->set_runnable();
        }
        run_schedule_unlocked(core);
        rq.stats().preemptions++;
        stats_.preemptions++;
    }
    /* Caller holds runqueues_[core].lock, or core is -1 and holds nothing. */
    void yield_unlocked(Task* t, int32_t core) {
        t->voluntary_switches++;
        t->set_runnable();
        if (core >= 0 && core < (int32_t)runqueues_.size()) {
            runqueues_[core].rotate();
            run_schedule_unlocked(core);
        }
    }
    /* Lock the runqueue a task is on -- Linux's task_rq_lock: read
       current_core, lock that queue, then confirm it did not change in
       between.  Returns the held lock; core_out is -1 (and nothing is held)
       when the task is on no queue. */
    std::unique_lock<std::mutex> lock_task_rq(Task* t, int32_t& core_out) {
        for (;;) {
            int32_t c = t->current_core;
            if (c < 0 || c >= (int32_t)runqueues_.size()) { core_out = -1; return {}; }
            std::unique_lock<std::mutex> lk(runqueues_[c].lock);
            if (t->current_core == c) { core_out = c; return lk; }
        }
    }

    bool enabled_;
    SchedulerConfig config_;
    /* std::deque, not vector: RunQueue holds a std::mutex, so it cannot be
       moved, and deque never relocates its elements. */
    std::deque<RunQueue> runqueues_;
    WaitQueue wait_queue_;
    std::vector<std::unique_ptr<Task>> tasks_;            /* host-heap tasks */
    std::vector<Task*>                 arena_owned_tasks_; /* arena-resident */
    KernelArena*                       arena_;             /* set via set_arena */
    std::unordered_map<int32_t, Task*> task_map_;
    /* Stage 5 (Apr 19 2026): reverse index keyed by the opaque
       app_thread_id (encode(app_id, thread_num)) so the kernel pthread's
       new_thread / thread_exit / quantum_expired dispatch can resolve
       the Task for the atid the simulator just handed us. */
    std::unordered_map<uint64_t, Task*> atid_map_;
    Stats stats_;
    /* Locking, Linux-style.  There is no global scheduler lock any more.
         rq.lock       -- each RunQueue's own; guards its deque, current_, stats
         tasks_lock_   -- tasks_, arena_owned_tasks_, task_map_, atid_map_
         wait_lock_    -- wait_queue_
       Order: an rq lock may be held while taking wait_lock_ (block), and is
       always released before tasks_lock_ (mark_dead).  Neither of the other
       two is ever held while taking an rq lock; wake_blocked drains the wait
       queue under wait_lock_ and wakes only after releasing it.  Public
       entry points lock; *_unlocked internals assume the rq lock is held,
       which is what removes the need for a recursive mutex. */
    mutable std::mutex tasks_lock_;
    mutable std::mutex wait_lock_;

public:

    /* ========================================================
       Stage 5 (Apr 19 2026) — Task ↔ atid bridge
       ========================================================
       These helpers are the narrow surface the kernel-pthread
       dispatch loop (mimicos.cc::poll_for_signal) uses.  All
       callers take `lock_` around whatever they do. */

    /**
     * Create a Task representing a live-app thread identified by its
     * opaque `atid` (== encode(app_id, thread_num)).  Returns nullptr
     * if `atid` is already registered (caller should use
     * find_task_by_atid instead).  Optionally wakes the task on
     * `preferred_core` so it's immediately eligible to dispatch.
     */
    Task* create_task_for_atid(const std::string& name,
                               uint64_t atid,
                               int32_t preferred_core = 0)
    {
        std::lock_guard<std::mutex> lk(tasks_lock_);
        auto it = atid_map_.find(atid);
        if (it != atid_map_.end()) return nullptr;
        Task* t = create_task_unlocked(name, preferred_core);
        if (!t) return nullptr;
        t->app_thread_id = atid;
        /* Pin to the core that received the new_thread event; callers may
           widen affinity_mask afterwards if migration is wanted. */
        if (preferred_core >= 0 && preferred_core < 64) {
            t->affinity_mask = (1ULL << preferred_core);
        }
        atid_map_[atid] = t;
        return t;
    }

    /**
     * Look up the Task for a given `atid`, or nullptr if unknown.
     */
    Task* find_task_by_atid(uint64_t atid) const {
        std::lock_guard<std::mutex> lk(tasks_lock_);
        auto it = atid_map_.find(atid);
        return it == atid_map_.end() ? nullptr : it->second;
    }

    /**
     * Mark a live-app task as DEAD: remove from its runqueue, drop
     * from atid_map_, and release the RunQueue::current slot if it
     * was there.  The Task object itself lives on in tasks_ /
     * arena_owned_tasks_ until shutdown — we don't free mid-run to
     * keep pointer identity stable for in-flight stats updates.
     */
    void mark_dead(Task* t) {
        if (!enabled_ || !t) return;
        {
            int32_t core = -1;
            std::unique_lock<std::mutex> rl = lock_task_rq(t, core);
            if (core >= 0) runqueues_[core].dequeue(t);
            t->set_dead();
        }   /* rq lock released before tasks_lock_: never nested */
        if (t->app_thread_id != 0) {
            std::lock_guard<std::mutex> lk(tasks_lock_);
            atid_map_.erase(t->app_thread_id);
        }
    }

    /**
     * After manipulating runqueues (wake_up / mark_dead / yield),
     * call this to select the next task and advance RunQueue::current.
     * Returns the newly-current Task, or nullptr if the core's
     * runqueue is empty.
     */
    Task* schedule_and_get_current(int32_t core) {
        if (!enabled_ || core < 0 || core >= (int32_t)runqueues_.size())
            return nullptr;
        std::lock_guard<std::mutex> lk(runqueues_[core].lock);
        run_schedule_unlocked(core);
        return runqueues_[core].get_current();
    }

private:
    /* Unlocked variants for internal use when lock_ is already held. */

    Task* create_task_unlocked(const std::string& name,
                               int32_t /*preferred_core*/)
    {
        int32_t id = static_cast<int32_t>(tasks_.size() + arena_owned_tasks_.size());
        Task* ptr;
        if (arena_ && arena_->enabled()) {
            ptr = arena_->alloc<Task>(KArenaSlab::TASK);
            arena_owned_tasks_.push_back(ptr);
        } else {
            auto task = std::make_unique<Task>();
            ptr = task.get();
            tasks_.push_back(std::move(task));
        }
        ptr->task_id = id;
        ptr->name = name;
        ptr->state = TaskState::CREATED;
        ptr->sched.timeslice_ns = config_.default_timeslice_ns;
        ptr->sched.timeslice_remaining_ns = config_.default_timeslice_ns;
        task_map_[id] = ptr;
        return ptr;
    }

    void run_schedule_unlocked(int32_t core) {
        RunQueue& rq = runqueues_[core];
        Task* next = nullptr;
        switch (config_.policy) {
        case SchedPolicy::FIFO_SIMPLE: next = rq.pick_next(); break;
        case SchedPolicy::RR_SIMPLE:
            next = rq.pick_next();
            if (next) next->sched.timeslice_remaining_ns = config_.default_timeslice_ns;
            break;
        case SchedPolicy::CFS_LITE:  next = rq.pick_next_min_vruntime(); break;
        case SchedPolicy::VM_AWARE:  next = pick_vm_aware(rq); break;
        default: break;
        }
        if (next && next != rq.get_current()) {
            Task* prev = rq.get_current();
            if (prev && prev->state == TaskState::RUNNING) prev->set_runnable();
            rq.set_current(next);
            rq.stats().context_switches++;
            stats_.context_switches++;
        }
    }
};
