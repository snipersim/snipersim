#pragma once
//
// RecencyPointerTable – Radix-based in-memory table for recency list pointers
//
// Stores prev_vpn / next_vpn for each VPN in a separate 4-level radix
// tree (NOT in the application page table).  Each pointer is 64 bits.
// Walking the tree to read an entry produces 4 physical addresses,
// one per radix level; the caller issues cache accesses at those
// addresses so the latency reflects real cache-hierarchy behavior.
//
// Physical layout (per core / per application):
//   Level 3 (root) : indexed by VPN[35:27],   512 × 8B = 4KB pages
//   Level 2        : indexed by VPN[26:18],   512 × 8B = 4KB pages
//   Level 1        : indexed by VPN[17:9],    512 × 8B = 4KB pages
//   Level 0 (leaf) : indexed by VPN[8:0],     512 × 16B entries
//                    (each entry = prev_vpn 8B + next_vpn 8B)
//
// Pages are lazily allocated via a deterministic bump allocator that
// starts at a per-core base physical address chosen to avoid overlap
// with real application memory.
//

#include "fixed_types.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace ParametricDramDirectoryMSI
{

class RecencyPointerTable
{
public:
    // phys_base: start of the physical address region reserved for
    //            this core's pointer-table pages.
    explicit RecencyPointerTable(uint64_t phys_base)
        : m_phys_base(phys_base),
          m_next_alloc(phys_base)
    {
        // Pre-allocate the root page (level 3).
        m_root_phys = allocPage();
    }

    // ── Read path ────────────────────────────────────────────────
    //
    // Returns the sequence of physical addresses that must be read
    // when walking the radix tree for `vpn`.  The vector always has
    // exactly NUM_LEVELS entries (root → leaf).  If an intermediate
    // page hasn't been allocated yet the corresponding entry will be
    // 0 (caller should skip the cache access and treat it as fast).
    //
    void getWalkAddresses(uint64_t vpn, std::vector<IntPtr> &addrs) const
    {
        addrs.clear();
        addrs.reserve(NUM_LEVELS);

        // Level 3 (root) — always exists
        uint64_t idx3 = extractIndex(vpn, 3);
        addrs.push_back(static_cast<IntPtr>(m_root_phys + idx3 * DIR_ENTRY_SIZE));

        // Level 2
        uint64_t key2 = packKey(2, vpn >> (BITS_PER_DIR_LEVEL * 1 + BITS_LEAF_LEVEL));
        auto it2 = m_level_pages.find(key2);
        if (it2 == m_level_pages.end()) { addrs.push_back(0); addrs.push_back(0); addrs.push_back(0); return; }
        uint64_t idx2 = extractIndex(vpn, 2);
        addrs.push_back(static_cast<IntPtr>(it2->second + idx2 * DIR_ENTRY_SIZE));

        // Level 1
        uint64_t key1 = packKey(1, vpn >> BITS_LEAF_LEVEL);
        auto it1 = m_level_pages.find(key1);
        if (it1 == m_level_pages.end()) { addrs.push_back(0); addrs.push_back(0); return; }
        uint64_t idx1 = extractIndex(vpn, 1);
        addrs.push_back(static_cast<IntPtr>(it1->second + idx1 * DIR_ENTRY_SIZE));

        // Level 0 (leaf)
        uint64_t leaf_key = packKey(0, vpn >> BITS_LEAF_LEVEL);
        auto it0 = m_level_pages.find(leaf_key);
        if (it0 == m_level_pages.end()) { addrs.push_back(0); return; }
        uint64_t idx0 = extractIndex(vpn, 0);
        addrs.push_back(static_cast<IntPtr>(it0->second + idx0 * LEAF_ENTRY_SIZE));
    }

    // ── Write path ───────────────────────────────────────────────
    //
    // Ensures all radix levels are allocated for `vpn`.  Called when
    // the recency list is mutated (pushFront, remove, notifyVictim)
    // so future reads return valid physical addresses.
    //
    void ensureAllocated(uint64_t vpn)
    {
        // Level 2 page
        uint64_t prefix2 = vpn >> (BITS_PER_DIR_LEVEL * 1 + BITS_LEAF_LEVEL);
        uint64_t key2 = packKey(2, prefix2);
        if (m_level_pages.find(key2) == m_level_pages.end())
            m_level_pages[key2] = allocPage();

        // Level 1 page
        uint64_t prefix1 = vpn >> BITS_LEAF_LEVEL;
        uint64_t key1 = packKey(1, prefix1);
        if (m_level_pages.find(key1) == m_level_pages.end())
            m_level_pages[key1] = allocPage();

        // Level 0 (leaf) page
        uint64_t leaf_key = packKey(0, prefix1);
        if (m_level_pages.find(leaf_key) == m_level_pages.end())
            m_level_pages[leaf_key] = allocPage();
    }

    // ── Accessors ────────────────────────────────────────────────

    uint64_t getAllocatedBytes() const { return m_next_alloc - m_phys_base; }

private:
    // Radix tree geometry
    static constexpr int NUM_LEVELS         = 4;   // levels 3 (root) → 0 (leaf)
    static constexpr int BITS_PER_DIR_LEVEL = 9;   // levels 3, 2, 1
    static constexpr int BITS_LEAF_LEVEL    = 9;   // level 0
    // Total VPN bits covered: 9+9+9+9 = 36 (sufficient for 48-bit VA / 4KB pages)
    static constexpr int DIR_ENTRY_SIZE     = 8;   // 8 bytes per directory pointer
    static constexpr int LEAF_ENTRY_SIZE    = 16;  // prev_vpn (8B) + next_vpn (8B)
    static constexpr int PAGE_SIZE          = 4096;

    uint64_t m_phys_base;
    uint64_t m_next_alloc;
    uint64_t m_root_phys;   // physical address of the root (level 3) page

    // Allocated pages: key = packKey(level, prefix) → physical base address
    std::unordered_map<uint64_t, uint64_t> m_level_pages;

    uint64_t allocPage()
    {
        uint64_t addr = m_next_alloc;
        m_next_alloc += PAGE_SIZE;
        return addr;
    }

    static uint64_t packKey(int level, uint64_t prefix)
    {
        return (static_cast<uint64_t>(level) << 48) | (prefix & 0x0000FFFFFFFFFFFF);
    }

    // Extract the index into the page at the given level.
    //   level 3: VPN[35:27]   (9 bits)
    //   level 2: VPN[26:18]   (9 bits)
    //   level 1: VPN[17:9]    (9 bits)
    //   level 0: VPN[8:0]     (9 bits)
    static uint64_t extractIndex(uint64_t vpn, int level)
    {
        int shift = 0;
        switch (level)
        {
        case 3: shift = BITS_PER_DIR_LEVEL * 2 + BITS_LEAF_LEVEL; break; // 27
        case 2: shift = BITS_PER_DIR_LEVEL * 1 + BITS_LEAF_LEVEL; break; // 18
        case 1: shift = BITS_LEAF_LEVEL;                           break; //  9
        case 0: shift = 0;                                         break; //  0
        }
        int bits = (level == 0) ? BITS_LEAF_LEVEL : BITS_PER_DIR_LEVEL;
        return (vpn >> shift) & ((1ULL << bits) - 1);
    }
};

} // namespace ParametricDramDirectoryMSI
