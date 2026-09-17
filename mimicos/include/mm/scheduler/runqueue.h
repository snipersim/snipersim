/**
 * @file runqueue.h
 * @brief Per-core run queue for MimicOS scheduling.
 *
 * Each simulated core has a RunQueue that maintains the set of
 * runnable tasks assigned to it.  The scheduler policies operate
 * on RunQueues to pick the next task.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <algorithm>
#include <functional>
#include <mutex>
#include <atomic>

#include "mm/scheduler/task.h"

/* ------------------------------------------------------------------ */
/*  Wait Queue (for blocked tasks)                                     */
/* ------------------------------------------------------------------ */

class WaitQueue {
public:
    void enqueue(Task* t) {
        waiters_.push_back(t);
    }

    Task* dequeue() {
        if (waiters_.empty()) return nullptr;
        Task* t = waiters_.front();
        waiters_.pop_front();
        return t;
    }

    /**
     * Wake all tasks waiting for a specific channel.
     * Returns number of tasks woken.
     */
    int wake_all(WaitChannel channel, std::function<void(Task*)> on_wake) {
        int woken = 0;
        auto it = waiters_.begin();
        while (it != waiters_.end()) {
            if ((*it)->wait_channel == channel) {
                Task* t = *it;
                it = waiters_.erase(it);
                on_wake(t);
                woken++;
            } else {
                ++it;
            }
        }
        return woken;
    }

    /**
     * Wake one task waiting for a specific channel.
     */
    Task* wake_one(WaitChannel channel) {
        for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
            if ((*it)->wait_channel == channel) {
                Task* t = *it;
                waiters_.erase(it);
                return t;
            }
        }
        return nullptr;
    }

    size_t size() const { return waiters_.size(); }
    bool empty() const { return waiters_.empty(); }

private:
    std::deque<Task*> waiters_;
};

/* ------------------------------------------------------------------ */
/*  Run Queue                                                          */
/* ------------------------------------------------------------------ */

class RunQueue {
public:
    RunQueue() : core_id_(-1), current_(nullptr), nr_running_(0) {}

    explicit RunQueue(int32_t core_id)
        : core_id_(core_id), current_(nullptr), nr_running_(0) {}

    /* One lock per runqueue, as in Linux's rq->lock.  A core scheduling on
       its own queue takes a lock nobody else holds, which is an uncontended
       CAS with no futex behind it; only a wake-up aimed at another core
       touches another core's lock.  Mutable so const readers can lock. */
    mutable std::mutex lock;

    /* ---- Task management ---- */

    void enqueue(Task* t) {
        t->set_runnable();
        tasks_.push_back(t);
        nr_running_++;
    }

    /**
     * Remove a task from the runqueue (e.g. when blocking).
     */
    void dequeue(Task* t) {
        auto it = std::find(tasks_.begin(), tasks_.end(), t);
        if (it != tasks_.end()) {
            tasks_.erase(it);
            nr_running_--;
        }
        if (current_ == t) current_ = nullptr;
    }

    /**
     * Pick the next task to run. Depends on policy (set externally).
     * Default: return front of queue (FIFO).
     */
    Task* pick_next() {
        if (tasks_.empty()) return nullptr;
        return tasks_.front();
    }

    /**
     * Pick next by minimum vruntime (CFS-like).
     */
    Task* pick_next_min_vruntime() {
        if (tasks_.empty()) return nullptr;
        return *std::min_element(tasks_.begin(), tasks_.end(),
            [](const Task* a, const Task* b) {
                return a->sched.vruntime < b->sched.vruntime;
            });
    }

    /**
     * Rotate: move current to back of queue (round-robin).
     */
    void rotate() {
        if (tasks_.size() <= 1) return;
        Task* front = tasks_.front();
        tasks_.pop_front();
        tasks_.push_back(front);
    }

    /* ---- Context switch ---- */

    void set_current(Task* t) {
        current_ = t;
        if (t) t->set_running(core_id_);
    }

    Task* get_current() const { return current_; }

    /* ---- Queries ---- */

    int32_t core_id() const { return core_id_; }
    size_t  length() const { return tasks_.size(); }
    /* Lockless, for load comparison across cores -- Linux reads other
       CPUs' nr_running the same way and tolerates it being a moment stale. */
    uint32_t nr_running() const { return nr_running_.load(std::memory_order_relaxed); }
    bool    empty() const { return tasks_.empty(); }

    const std::deque<Task*>& tasks() const { return tasks_; }

    /* ---- Stats ---- */

    struct Stats {
        uint64_t context_switches    = 0;
        uint64_t preemptions         = 0;
        uint64_t idle_periods        = 0;
        uint64_t total_wait_time_ns  = 0;
    };

    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

private:
    int32_t core_id_;
    Task* current_;
    std::atomic<uint32_t> nr_running_;
    std::deque<Task*> tasks_;
    Stats stats_;
};
