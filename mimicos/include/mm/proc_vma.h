/**
 * @file proc_vma.h
 * @brief Live VMA tree backed by /proc/<pid>/maps (Stage C v2, Apr 16 2026).
 *
 * MimicOS reads the real OS's VMA state for spawned live processes via
 * /proc/<pid>/maps.  The tree is cached in-memory and refreshed lazily:
 *   - Once at first kernel entry (after initial SimContextSwitch).
 *   - On cache miss (faulting addr not found → app did a new mmap).
 *
 * All /proc reads happen OUTSIDE the rdtsc-bracketed fault-latency
 * measurement window, so they don't pollute per-fault timing.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <sys/types.h>

struct ProcVma {
    uint64_t    start = 0;       // inclusive
    uint64_t    end   = 0;       // exclusive
    int         prot  = 0;       // PROT_READ | PROT_WRITE | PROT_EXEC
    int         flags = 0;       // MAP_PRIVATE / MAP_SHARED
    uint64_t    offset = 0;
    std::string path;            // mapped file path (empty for anon)

    uint64_t size()     const { return end - start; }
    bool contains(uint64_t a) const { return a >= start && a < end; }
    bool is_anon()      const { return path.empty() || path[0] != '/'; }
};

class ProcVmaTree {
public:
    ProcVmaTree() = default;

    /** Refresh the entire tree from /proc/<pid>/maps.
     *  Returns number of VMAs parsed, or -1 on error. */
    int refresh(pid_t pid);

    /** Look up the VMA containing `addr`.
     *  Returns nullptr if not found (caller should refresh + retry). */
    const ProcVma* find(uint64_t addr) const;

    /** Number of cached VMAs. */
    size_t size() const { return m_vmas.size(); }

    /** Dump a human-readable summary to a file. */
    void dump(const std::string &path) const;

    /** Access all VMAs for iteration. */
    const std::map<uint64_t, ProcVma>& vmas() const { return m_vmas; }

private:
    pid_t m_pid = 0;
    std::map<uint64_t, ProcVma> m_vmas;  // keyed by start addr
};
