#pragma once

/*
 * Utopia physical memory allocator
 *
 *   "Utopia: Fast and Efficient Address Translation via Hybrid Restrictive &
 *    Flexible Virtual-to-Physical Address Mappings"
 *   Kanellopoulos et al., MICRO 2023 (https://doi.org/10.1145/3613424.3623789)
 *
 * Physical memory is split in two kinds of segment:
 *
 *   RestSeg (restrictive segment): a set-associative structure of page frames
 *     with one fixed page size.  A virtual page may only live in the set its
 *     VPN selects, in one of `assoc` ways, so its physical address follows
 *     from (set, way) alone:
 *
 *         ppn = base + (set * assoc + way) * (page_size / 4KB)
 *
 *     and no page table walk is needed to translate it.  The hardware keeps,
 *     per core, a fingerprint array (FPA, one byte per way: a CRC-8 of the
 *     tag) and a tag array (TAR); the simulator also keeps a per-application
 *     radix "way table" (VPN -> {valid, way}) that the MMU can walk instead
 *     of FPA+TAR.  The RestSeg frames, the FPA/TAR and the way-table nodes
 *     are carved from the kernel region of the simulated physical memory so
 *     that their accesses can be modelled through the cache hierarchy.
 *
 *   FlexSeg (flexible segment): the buddy allocator, with the reservation
 *     based THP scheme of reserve_thp.h on top (a 2MB region is reserved on
 *     the first touch and promoted once its utilisation reaches the
 *     threshold).  FlexSeg pages are translated through the page table.
 *
 * Allocation hierarchy, in order of preference:
 *   1. RestSeg-2MB   (no walk, 2MB page)
 *   2. FlexSeg-THP   (page table, 2MB region reserved; 4KB until promoted)
 *   3. RestSeg-4KB   (no walk, 4KB page)
 *   4. FlexSeg-4KB   (page table, buddy 4KB page)
 * A first-touch allocation never evicts from a RestSeg; only an explicit
 * migration (migratePage, driven by the MMU's cost-based policy) does.
 *
 * The allocator is a template over a simulator policy (configuration reads,
 * statistics registration, logging and the page-table-entry deletion a
 * migration needs), like the other allocators in this directory; the
 * BuddyPolicyFor trait names the buddy's policy.  `Utopia` at the bottom is
 * the Sniper instantiation.
 */

#include "debug_config.h"
#include "sim_log.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "templates_traits_config.h"
#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"
#include "memory_management/physical_memory_allocators/buddy.h"

#include <cmath>
#include <vector>
#include <array>
#include <unordered_map>
#include <map>
#include <tuple>
#include <bitset>
#include <mutex>
#include <algorithm>
#include <memory>
#include <functional>
#include <cassert>

// ============================================================================
// Fingerprints
// ============================================================================

// CRC-8, polynomial 0x07.  The fingerprint of a RestSeg entry is the low
// `num_bits` of the CRC of its tag, and the FPA stores one per way so a set
// can be filtered before the tag array is read.
static const uint8_t utopia_crc8_table[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3
};

inline uint8_t utopiaFingerprint(UInt64 tag, int num_bits)
{
    uint8_t crc = 0;
    for (int i = 0; i < 8; i++)
    {
        if ((tag >> (i * 8)) == 0)
            break;   // no significant bytes left
        crc = utopia_crc8_table[crc ^ (uint8_t)((tag >> (i * 8)) & 0xFF)];
    }
    return crc & (uint8_t)((1 << num_bits) - 1);
}

// ============================================================================
// Radix way table: per application, VPN -> {valid, way}
// ============================================================================
//
// A sparse radix tree.  Internal nodes have 512 children; a leaf is one 4KB
// page of packed {valid:1, way:log2(assoc)} entries, so a leaf holds
// floor(32768 / (1 + way_bits)) entries -- not a power of two, hence the
// division/modulo indexing.  The levels above the leaf index vpn / leaf_entries
// nine bits at a time.  Every node has a simulated physical address (a frame
// from the kernel region) so that the MMU can charge one cache-line read per
// level of a lookup.

struct UtopiaRadixNode
{
    static constexpr int RADIX_BITS = 9;
    static constexpr int FANOUT = 1 << RADIX_BITS;

    std::array<UtopiaRadixNode *, FANOUT> children;
    bool is_leaf;
    IntPtr sim_phys_addr;

    UtopiaRadixNode() : is_leaf(false), sim_phys_addr(0) { children.fill(nullptr); }
    virtual ~UtopiaRadixNode()
    {
        if (!is_leaf)
            for (auto *child : children)
                delete child;
    }
};

class UtopiaRadixLeaf : public UtopiaRadixNode
{
public:
    static constexpr int LEAF_BYTES = 4096;
    static constexpr int LEAF_BITS = LEAF_BYTES * 8;
    static constexpr int CACHE_LINE_BYTES = 64;

private:
    int way_bits;
    int bits_per_entry;
    int leaf_entries;
    std::array<uint8_t, LEAF_BYTES> data;

public:
    explicit UtopiaRadixLeaf(int _way_bits) : way_bits(_way_bits), bits_per_entry(1 + _way_bits)
    {
        is_leaf = true;
        leaf_entries = LEAF_BITS / bits_per_entry;
        data.fill(0);
    }

    int getLeafEntries() const { return leaf_entries; }

    // An entry is at most 8 bits (7 way bits) and so spans at most two bytes;
    // the third byte is read for an entry that starts late in a byte.
    void getEntry(int index, bool &valid, int &way) const
    {
        if (index < 0 || index >= leaf_entries)
        {
            valid = false;
            way = -1;
            return;
        }
        int bit_pos = index * bits_per_entry;
        int byte_pos = bit_pos / 8;
        int bit_offset = bit_pos % 8;
        uint32_t raw = data[byte_pos];
        if (byte_pos + 1 < LEAF_BYTES)
            raw |= (uint32_t)data[byte_pos + 1] << 8;
        if (byte_pos + 2 < LEAF_BYTES && bit_offset + bits_per_entry > 16)
            raw |= (uint32_t)data[byte_pos + 2] << 16;
        raw >>= bit_offset;
        uint32_t entry = raw & ((1u << bits_per_entry) - 1);
        valid = (entry >> way_bits) & 1;
        way = entry & ((1 << way_bits) - 1);
    }

    void setEntry(int index, bool valid, int way)
    {
        if (index < 0 || index >= leaf_entries)
            return;
        uint32_t entry = (way & ((1 << way_bits) - 1)) | (valid ? (1u << way_bits) : 0);
        int bit_pos = index * bits_per_entry;
        int byte_pos = bit_pos / 8;
        int bit_offset = bit_pos % 8;
        uint32_t mask = (1u << bits_per_entry) - 1;
        uint32_t clear_mask = ~(mask << bit_offset);
        uint32_t set_mask = (entry & mask) << bit_offset;
        data[byte_pos] = (data[byte_pos] & (clear_mask & 0xFF)) | (set_mask & 0xFF);
        if (byte_pos + 1 < LEAF_BYTES)
            data[byte_pos + 1] = (data[byte_pos + 1] & ((clear_mask >> 8) & 0xFF)) | ((set_mask >> 8) & 0xFF);
        if (byte_pos + 2 < LEAF_BYTES && bit_offset + bits_per_entry > 16)
            data[byte_pos + 2] = (data[byte_pos + 2] & ((clear_mask >> 16) & 0xFF)) | ((set_mask >> 16) & 0xFF);
    }

    void clearEntry(int index) { setEntry(index, false, 0); }

    // Byte offset, within the leaf, of the cache line holding an entry
    int getCacheLineOffset(int index) const
    {
        int byte_pos = (index * bits_per_entry) / 8;
        return (byte_pos / CACHE_LINE_BYTES) * CACHE_LINE_BYTES;
    }
};

class UtopiaRadixWayTable
{
public:
    static constexpr int RADIX_BITS = UtopiaRadixNode::RADIX_BITS;
    static constexpr int MAX_LEVELS = 4;
    static constexpr int LEAF_BITS = UtopiaRadixLeaf::LEAF_BITS;

private:
    UtopiaRadixNode *root = nullptr;
    int num_internal_levels;
    int way_bits;
    int leaf_entries;
    int page_size_bits;
    mutable std::mutex table_lock;

    UInt64 nodes_allocated = 0;
    UInt64 leaves_allocated = 0;

    std::function<IntPtr(size_t)> alloc_phys_page;   // simulated frame for a new node

    static int wayBitsFor(int associativity)
    {
        int bits = 0;
        for (int t = associativity; t > 1; t >>= 1)
            bits++;
        return bits == 0 ? 1 : bits;
    }

public:
    UtopiaRadixWayTable(int _page_size_bits, int associativity, std::function<IntPtr(size_t)> _alloc_phys_page)
        : page_size_bits(_page_size_bits), alloc_phys_page(_alloc_phys_page)
    {
        way_bits = wayBitsFor(associativity);
        leaf_entries = LEAF_BITS / (1 + way_bits);
        num_internal_levels = internalLevelsFor(page_size_bits, leaf_entries);
    }
    ~UtopiaRadixWayTable() { delete root; }
    UtopiaRadixWayTable(const UtopiaRadixWayTable &) = delete;
    UtopiaRadixWayTable &operator=(const UtopiaRadixWayTable &) = delete;

    // Levels needed above the leaf to index a 48-bit virtual address space
    static int internalLevelsFor(int page_size_bits, int leaf_entries)
    {
        UInt64 max_vpn = 1ULL << (48 - page_size_bits);
        UInt64 max_vpn_above_leaf = (max_vpn + leaf_entries - 1) / leaf_entries;
        int bits_needed = 0;
        for (UInt64 t = max_vpn_above_leaf; t > 0; t >>= 1)
            bits_needed++;
        int levels = (bits_needed + RADIX_BITS - 1) / RADIX_BITS;
        if (levels > MAX_LEVELS) levels = MAX_LEVELS;
        if (levels < 1) levels = 1;
        return levels;
    }

    int getLeafEntries() const { return leaf_entries; }
    int getNumLevels() const { return num_internal_levels + 1; }
    int getWayBits() const { return way_bits; }
    UInt64 getNodesAllocated() const { return nodes_allocated; }
    UInt64 getLeavesAllocated() const { return leaves_allocated; }

    void computeIndices(UInt64 vpn, int &leaf_index, UInt64 &vpn_above_leaf) const
    {
        leaf_index = vpn % leaf_entries;
        vpn_above_leaf = vpn / leaf_entries;
    }

    int getLevelIndex(UInt64 vpn_above_leaf, int level) const
    {
        int shift = (num_internal_levels - 1 - level) * RADIX_BITS;
        return (vpn_above_leaf >> shift) & ((1 << RADIX_BITS) - 1);
    }

    bool lookup_way(UInt64 vpn, int &way_out) const
    {
        std::lock_guard<std::mutex> guard(table_lock);
        way_out = -1;
        if (!root)
            return false;
        int leaf_index;
        UInt64 vpn_above_leaf;
        computeIndices(vpn, leaf_index, vpn_above_leaf);
        UtopiaRadixNode *node = root;
        for (int level = 0; level < num_internal_levels; level++)
        {
            node = node->children[getLevelIndex(vpn_above_leaf, level)];
            if (!node)
                return false;
        }
        if (!node->is_leaf)
            return false;
        bool valid;
        static_cast<UtopiaRadixLeaf *>(node)->getEntry(leaf_index, valid, way_out);
        return valid;
    }

    void set_mapping(UInt64 vpn, int way)
    {
        std::lock_guard<std::mutex> guard(table_lock);
        int leaf_index;
        UInt64 vpn_above_leaf;
        computeIndices(vpn, leaf_index, vpn_above_leaf);
        if (!root)
        {
            root = new UtopiaRadixNode();
            if (alloc_phys_page)
                root->sim_phys_addr = alloc_phys_page(4096);
            nodes_allocated++;
        }
        UtopiaRadixNode *node = root;
        for (int level = 0; level < num_internal_levels; level++)
        {
            int index = getLevelIndex(vpn_above_leaf, level);
            if (!node->children[index])
            {
                UtopiaRadixNode *child;
                if (level == num_internal_levels - 1)
                {
                    child = new UtopiaRadixLeaf(way_bits);
                    leaves_allocated++;
                }
                else
                {
                    child = new UtopiaRadixNode();
                    nodes_allocated++;
                }
                if (alloc_phys_page)
                    child->sim_phys_addr = alloc_phys_page(4096);
                node->children[index] = child;
            }
            node = node->children[index];
        }
        static_cast<UtopiaRadixLeaf *>(node)->setEntry(leaf_index, true, way);
    }

    void clear_mapping(UInt64 vpn)
    {
        std::lock_guard<std::mutex> guard(table_lock);
        if (!root)
            return;
        int leaf_index;
        UInt64 vpn_above_leaf;
        computeIndices(vpn, leaf_index, vpn_above_leaf);
        UtopiaRadixNode *node = root;
        for (int level = 0; level < num_internal_levels; level++)
        {
            node = node->children[getLevelIndex(vpn_above_leaf, level)];
            if (!node)
                return;
        }
        if (node->is_leaf)
            static_cast<UtopiaRadixLeaf *>(node)->clearEntry(leaf_index);
    }

    // The cache lines a lookup reads, root first: (level, line address,
    // is_leaf).  Stops where the tree does.
    void getTraversalAddresses(UInt64 vpn, std::vector<std::tuple<int, IntPtr, bool>> &addresses) const
    {
        std::lock_guard<std::mutex> guard(table_lock);
        addresses.clear();
        if (!root)
            return;
        int leaf_index;
        UInt64 vpn_above_leaf;
        computeIndices(vpn, leaf_index, vpn_above_leaf);
        UtopiaRadixNode *node = root;
        for (int level = 0; level < num_internal_levels; level++)
        {
            int index = getLevelIndex(vpn_above_leaf, level);
            IntPtr ptr_addr = node->sim_phys_addr + index * 8;
            addresses.push_back(std::make_tuple(level, ptr_addr & ~(IntPtr)63, false));
            node = node->children[index];
            if (!node)
                return;
        }
        if (node->is_leaf)
        {
            UtopiaRadixLeaf *leaf = static_cast<UtopiaRadixLeaf *>(node);
            addresses.push_back(std::make_tuple(num_internal_levels, leaf->sim_phys_addr + leaf->getCacheLineOffset(leaf_index), true));
        }
    }
};

// One way table per application, created on demand
class UtopiaRadixWayTableManager
{
private:
    std::unordered_map<int, std::unique_ptr<UtopiaRadixWayTable>> app_tables;
    int page_size_bits;
    int associativity;
    int leaf_entries;
    mutable std::mutex manager_lock;
    std::function<IntPtr(size_t)> alloc_phys_page;

public:
    UtopiaRadixWayTableManager(int _page_size_bits, int _associativity, std::function<IntPtr(size_t)> _alloc_phys_page)
        : page_size_bits(_page_size_bits), associativity(_associativity), alloc_phys_page(_alloc_phys_page)
    {
        int way_bits = 0;
        for (int t = associativity; t > 1; t >>= 1)
            way_bits++;
        if (way_bits == 0) way_bits = 1;
        leaf_entries = UtopiaRadixWayTable::LEAF_BITS / (1 + way_bits);
    }

    UtopiaRadixWayTable *getTable(int app_id)
    {
        std::lock_guard<std::mutex> guard(manager_lock);
        auto it = app_tables.find(app_id);
        if (it != app_tables.end())
            return it->second.get();
        auto table = std::make_unique<UtopiaRadixWayTable>(page_size_bits, associativity, alloc_phys_page);
        UtopiaRadixWayTable *ptr = table.get();
        app_tables[app_id] = std::move(table);
        return ptr;
    }

    bool hasTable(int app_id) const
    {
        std::lock_guard<std::mutex> guard(manager_lock);
        return app_tables.find(app_id) != app_tables.end();
    }

    bool lookup_way(UInt64 vpn, int app_id, int &way_out) { return getTable(app_id)->lookup_way(vpn, way_out); }
    void set_mapping(UInt64 vpn, int app_id, int way) { getTable(app_id)->set_mapping(vpn, way); }
    void clear_mapping(UInt64 vpn, int app_id)
    {
        if (hasTable(app_id))
            getTable(app_id)->clear_mapping(vpn);
    }
    int getNumLevels() const { return UtopiaRadixWayTable::internalLevelsFor(page_size_bits, leaf_entries) + 1; }
    int getLeafEntries() const { return leaf_entries; }
    void getTraversalAddresses(UInt64 vpn, int app_id, std::vector<std::tuple<int, IntPtr, bool>> &addresses)
    {
        getTable(app_id)->getTraversalAddresses(vpn, addresses);
    }
};

// ============================================================================
// RestSeg set
// ============================================================================

struct RestSegEntry
{
    bool valid = false;
    UInt64 tag = 0;          // VPN / num_sets
    uint8_t fingerprint = 0;
    UInt64 owner = 0;        // app_id + 1; 0 is free
    UInt64 reuse_count = 0;
    UInt64 lru_counter = 0;  // higher is more recent

    void invalidate()
    {
        valid = false;
        tag = 0;
        fingerprint = 0;
        owner = 0;
        reuse_count = 0;
    }
};

class RestSegSet
{
private:
    std::vector<RestSegEntry> entries;
    UInt32 associativity;
    UInt64 global_lru_counter = 0;

    void fill(int way, UInt64 tag, UInt64 owner, uint8_t fingerprint)
    {
        entries[way].valid = true;
        entries[way].tag = tag;
        entries[way].fingerprint = fingerprint;
        entries[way].owner = owner;
        entries[way].lru_counter = ++global_lru_counter;
        entries[way].reuse_count = 0;
    }

public:
    RestSegSet(UInt32 assoc) : associativity(assoc) { entries.resize(assoc); }

    UInt32 getAssociativity() const { return associativity; }
    RestSegEntry *getEntry(UInt32 way) { return (way < associativity) ? &entries[way] : nullptr; }
    const RestSegEntry *getEntry(UInt32 way) const { return (way < associativity) ? &entries[way] : nullptr; }

    // The way holding (tag, owner), or -1
    int findEntry(UInt64 tag, UInt64 owner) const
    {
        for (UInt32 i = 0; i < associativity; i++)
            if (entries[i].valid && entries[i].tag == tag && entries[i].owner == owner)
                return i;
        return -1;
    }

    // findEntry, also counting what a fingerprint filter would have said about
    // each valid way (shadow statistics; the lookup itself is on the tag)
    int findEntryWithFingerprintShadow(UInt64 tag, UInt64 owner, uint8_t fingerprint,
                                       UInt64 &fp_checks, UInt64 &fp_matches,
                                       UInt64 &fp_false_positives, UInt64 &fp_true_positives) const
    {
        int result = -1;
        for (UInt32 i = 0; i < associativity; i++)
        {
            if (!entries[i].valid)
                continue;
            fp_checks++;
            bool fp_match = (entries[i].fingerprint == fingerprint);
            bool tag_owner_match = (entries[i].tag == tag && entries[i].owner == owner);
            if (fp_match)
            {
                fp_matches++;
                if (tag_owner_match)
                    fp_true_positives++;
                else
                    fp_false_positives++;
            }
            if (tag_owner_match)
                result = i;
        }
        return result;
    }

    int findFreeWay() const
    {
        for (UInt32 i = 0; i < associativity; i++)
            if (!entries[i].valid)
                return i;
        return -1;
    }

    int findLRUVictim() const
    {
        int victim = -1;
        UInt64 min_lru = UINT64_MAX;
        for (UInt32 i = 0; i < associativity; i++)
            if (entries[i].valid && entries[i].lru_counter < min_lru)
            {
                min_lru = entries[i].lru_counter;
                victim = i;
            }
        return victim;
    }

    struct InsertResult
    {
        bool success = false;
        bool evicted = false;
        UInt64 evicted_tag = 0;
        UInt64 evicted_owner = 0;
        bool was_hit = false;        // the entry was already present
        bool used_free_way = false;  // a free way was consumed
        int used_way = -1;
    };

    // Insert (tag, owner): a hit refreshes the entry; else the first free way;
    // else, with force_evict, the LRU way is replaced and its (tag, owner)
    // reported so the caller can deal with the evicted page.
    InsertResult insert(UInt64 tag, UInt64 owner, uint8_t fingerprint, bool force_evict)
    {
        InsertResult r;
        int existing = findEntry(tag, owner);
        if (existing >= 0)
        {
            entries[existing].lru_counter = ++global_lru_counter;
            entries[existing].reuse_count++;
            r.success = true;
            r.was_hit = true;
            r.used_way = existing;
            return r;
        }
        int free_way = findFreeWay();
        if (free_way >= 0)
        {
            fill(free_way, tag, owner, fingerprint);
            r.success = true;
            r.used_free_way = true;
            r.used_way = free_way;
            return r;
        }
        if (!force_evict)
            return r;
        int victim = findLRUVictim();
        if (victim < 0)
            return r;
        r.evicted = true;
        r.evicted_tag = entries[victim].tag;
        r.evicted_owner = entries[victim].owner;
        fill(victim, tag, owner, fingerprint);
        r.success = true;
        r.used_way = victim;
        return r;
    }

    // Insert at one given way only: a hit anywhere in the set counts as
    // success, otherwise the way must be free.  No fallback, no eviction.
    bool tryInsertAtWay(UInt64 tag, UInt64 owner, uint8_t fingerprint, int way, int &used_way, bool &was_hit)
    {
        used_way = -1;
        was_hit = false;
        int existing = findEntry(tag, owner);
        if (existing >= 0)
        {
            entries[existing].lru_counter = ++global_lru_counter;
            entries[existing].reuse_count++;
            used_way = existing;
            was_hit = true;
            return true;
        }
        if (way < 0 || way >= (int)associativity || entries[way].valid)
            return false;
        fill(way, tag, owner, fingerprint);
        used_way = way;
        return true;
    }

    void touchEntry(int way)
    {
        if (way >= 0 && way < (int)associativity && entries[way].valid)
        {
            entries[way].lru_counter = ++global_lru_counter;
            entries[way].reuse_count++;
        }
    }

    UInt32 countValid() const
    {
        UInt32 count = 0;
        for (UInt32 i = 0; i < associativity; i++)
            if (entries[i].valid)
                count++;
        return count;
    }
};

// ============================================================================
// RestSeg
// ============================================================================
//
// Indexing: page_num = VA >> page_size_bits, tag = page_num / num_sets,
// set = (page_num % num_sets) ^ (tag % num_sets with its low nine bits
// cleared), folded modulo num_sets -- the XOR brings tag entropy into the
// set index so that a linear stride does not pile into one set, and leaving
// the low nine bits alone keeps the 512 pages of a 2MB-aligned region of a
// 4KB RestSeg in 512 consecutive sets.  Frames are laid out way-major: the
// physical page number of (set, way) is base + (way * num_sets + set) *
// (page_size / 4KB), in 4KB units, so that the frames of one way are
// contiguous across consecutive sets -- which is what lets 512 pages placed
// in the same way of consecutive sets form one contiguous, aligned 2MB
// page (the coalescing of utopia_hash_coalesce.h).  Upstream folds every
// index bit and lays the frames out set-major, under which such a page is
// not contiguous.
//
// Placement of a new page prefers, in order, the way its 1GB region used last
// (region_way_pref, 4KB RestSegs only), the global preferred way (rotated to
// the emptiest way when it congests), and any free way: this keeps the pages
// of a region in one way, which is what a way predictor can learn.

class RestSeg
{
private:
    int id;
    UInt64 size_bytes;
    int page_size_bits;
    UInt32 associativity;
    UInt32 num_sets;
    std::vector<RestSegSet> sets;
    IntPtr base_physical_address = 0;   // 4KB PPN of the segment's first frame

    // Statistics
    UInt64 accesses = 0;
    UInt64 hits = 0;
    UInt64 conflicts = 0;
    UInt64 allocations = 0;
    mutable UInt64 fingerprint_checks = 0;
    mutable UInt64 fingerprint_matches = 0;
    mutable UInt64 fingerprint_false_positives = 0;
    mutable UInt64 fingerprint_true_positives = 0;

    // Region way preference: one preferred way per 1GB region of 4KB pages
    static constexpr int RLE_REGION_BITS = 18;   // 2^18 4KB pages = 1GB
    static constexpr UInt32 REGION_PREF_FAILURE_THRESHOLD = 32;
    struct RegionWayPref
    {
        int way = -1;
        UInt32 failures = 0;
        UInt32 successes = 0;
        UInt64 total_allocations = 0;
        UInt64 rle_misses = 0;
    };
    std::unordered_map<UInt64, RegionWayPref> region_way_pref;
    mutable UInt64 region_way_attempts = 0;
    mutable UInt64 region_way_hits = 0;
    mutable UInt64 region_way_relearns = 0;

    // Global preferred way and its congestion-driven rotation
    std::vector<UInt32> free_sets_per_way;
    int global_way = 0;
    UInt64 global_way_rotations = 0;
    UInt64 global_way_attempts = 0;
    UInt64 global_way_hits = 0;
    UInt64 global_way_fallbacks = 0;
    double congestion_free_frac = 0.10;   // rotate when under 10% of the sets have this way free
    UInt32 rotate_cooldown = 0;
    UInt32 rotate_cooldown_max = 1024;    // at most one rotation per 1024 allocations

    mutable std::mutex lock;

    // Per-core metadata: the FPA and TAR of each core, in simulated memory
    std::vector<IntPtr> tag_bases;
    std::vector<IntPtr> fingerprint_bases;
    int tag_entry_bytes;
    int fingerprint_bits;
    bool fingerprint_enabled;

    // Per-application radix way table
    std::unique_ptr<UtopiaRadixWayTableManager> radix_way_tables;
    mutable UInt64 radix_lookups = 0;
    mutable UInt64 radix_hits = 0;
    mutable UInt64 radix_misses = 0;

    // Coalescing (utopia_hash_coalesce.h): per set, a bitmap of its free
    // ways (bit w set: way w free; up to 64 ways), and a second radix way
    // table keyed by the 2MB VPN for the regions placed whole in one way
    std::vector<uint64_t> set_way_bitmap;
    std::unique_ptr<UtopiaRadixWayTableManager> radix_way_tables_2mb;
    mutable UInt64 coalesced_2mb_lookups = 0;
    mutable UInt64 coalesced_2mb_hits = 0;

    void updateWayBitmap(UInt32 set_index, int way, bool is_free)
    {
        if (way < 0 || way >= (int)associativity || way >= 64 || set_index >= num_sets)
            return;
        if (is_free)
            set_way_bitmap[set_index] |= (1ULL << way);
        else
            set_way_bitmap[set_index] &= ~(1ULL << way);
    }

    mutable std::unique_ptr<SimLog> m_log;

    void fillRegionPref(bool success_from_region_pref, UInt64 region_id, int used_way)
    {
        auto &pref = region_way_pref[region_id];
        pref.total_allocations++;
        if (!success_from_region_pref)
            pref.rle_misses++;
        pref.way = used_way;
        pref.successes++;
        pref.failures = 0;
    }

    // Rotate the global way to the emptiest one once the current one congests
    void maybeRotateGlobalWay()
    {
        if (rotate_cooldown > 0)
        {
            rotate_cooldown--;
            return;
        }
        double free_frac = (double)free_sets_per_way[global_way] / (double)num_sets;
        if (free_frac >= congestion_free_frac)
            return;
        int best = global_way;
        UInt32 best_free = free_sets_per_way[global_way];
        int next_after_current = (global_way + 1) % associativity;
        for (int w = 0; w < (int)associativity; w++)
        {
            if (free_sets_per_way[w] > best_free)
            {
                best_free = free_sets_per_way[w];
                best = w;
            }
            else if (free_sets_per_way[w] == best_free && w != global_way && w == next_after_current)
                best = w;   // round-robin tie break
        }
        if (best != global_way)
        {
            global_way = best;
            global_way_rotations++;
            rotate_cooldown = rotate_cooldown_max;
        }
    }

public:
    RestSeg(int _id, UInt64 size_mb, int _page_size_bits, UInt32 _assoc, int num_cores,
            int _fingerprint_bits, bool _fingerprint_enabled)
        : id(_id), page_size_bits(_page_size_bits), associativity(_assoc),
          fingerprint_bits(_fingerprint_bits), fingerprint_enabled(_fingerprint_enabled)
    {
        size_bytes = size_mb * 1024ULL * 1024ULL;
        num_sets = size_bytes / (associativity * (1ULL << page_size_bits));
        assert(num_sets > 0);
        sets.reserve(num_sets);
        for (UInt32 i = 0; i < num_sets; i++)
            sets.emplace_back(associativity);

        // Tag bits = 48 - page offset bits - set index bits
        tag_entry_bytes = (48 - page_size_bits - (int)std::ceil(std::log2(num_sets))) / 8;
        if (tag_entry_bytes < 1) tag_entry_bytes = 1;

        tag_bases.resize(num_cores, 0);
        fingerprint_bases.resize(num_cores, 0);
        free_sets_per_way.assign(associativity, num_sets);
        radix_way_tables = std::make_unique<UtopiaRadixWayTableManager>(page_size_bits, associativity, nullptr);
        radix_way_tables_2mb = std::make_unique<UtopiaRadixWayTableManager>(21, associativity, nullptr);
        set_way_bitmap.assign(num_sets, (associativity >= 64) ? ~0ULL : ((1ULL << associativity) - 1));

        std::string page_size_str = (page_size_bits == 21) ? "2MB" : (page_size_bits == 12) ? "4KB" :
                                    (page_size_bits == 30) ? "1GB" : std::to_string(page_size_bits);
        m_log = std::make_unique<SimLog>("RestSeg-" + page_size_str, -1, DEBUG_UTOPIA);
    }

    // The way-table nodes take simulated frames from this allocator; set it
    // before the first allocation
    void setRadixAllocator(std::function<IntPtr(size_t)> alloc_fn)
    {
        radix_way_tables = std::make_unique<UtopiaRadixWayTableManager>(page_size_bits, associativity, alloc_fn);
        radix_way_tables_2mb = std::make_unique<UtopiaRadixWayTableManager>(21, associativity, alloc_fn);
    }

    static constexpr UInt32 FOLD_MASK = ~(UInt32)511;   // the index bits the tag is folded into

    void splitAddress(IntPtr address, UInt64 &tag, UInt32 &set_index) const
    {
        UInt64 page_num = address >> page_size_bits;
        tag = page_num / num_sets;
        UInt32 raw_index = page_num % num_sets;
        set_index = (raw_index ^ (((UInt32)tag % num_sets) & FOLD_MASK)) % num_sets;
    }

    // Inverse of splitAddress (exact for a power-of-two number of sets)
    IntPtr reconstructAddress(UInt64 tag, UInt32 set_index) const
    {
        UInt32 raw_index = (set_index ^ (((UInt32)tag % num_sets) & FOLD_MASK)) % num_sets;
        return (tag * num_sets + raw_index) << page_size_bits;
    }

    // ---- lookups -----------------------------------------------------------

    // Is the page in this RestSeg (tag-array truth)?  Counts an access, and
    // touches the LRU on a hit.
    bool inRestSeg(IntPtr address, int app_id, bool count_stats = true)
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        uint8_t fp = utopiaFingerprint(tag, fingerprint_bits);
        if (count_stats) accesses++;
        UInt64 fp_checks = 0, fp_matches = 0, fp_false_positives = 0, fp_true_positives = 0;
        int way = sets[set_index].findEntryWithFingerprintShadow(tag, app_id + 1, fp, fp_checks, fp_matches, fp_false_positives, fp_true_positives);
        if (count_stats)
        {
            fingerprint_checks += fp_checks;
            fingerprint_matches += fp_matches;
            fingerprint_false_positives += fp_false_positives;
            fingerprint_true_positives += fp_true_positives;
        }
        if (way >= 0)
        {
            if (count_stats) hits++;
            sets[set_index].touchEntry(way);
            return true;
        }
        return false;
    }

    // The way of the page, or -1; no statistics, no LRU update
    int lookupWay(IntPtr address, int app_id) const
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        return sets[set_index].findEntry(tag, app_id + 1);
    }
    int getWayForAddress(IntPtr address, int app_id) const { return lookupWay(address, app_id); }

    // The way of the page from the application's radix way table, or false
    bool lookupWayRadix(IntPtr address, int app_id, int &way_out) const
    {
        UInt64 vpn = address >> page_size_bits;
        radix_lookups++;
        bool found = false;
        if (page_size_bits == 12)
            found = radix_way_tables_2mb->lookup_way(address >> 21, app_id, way_out);
        if (!found)
            found = radix_way_tables->lookup_way(vpn, app_id, way_out);
        if (found)
            radix_hits++;
        else
            radix_misses++;
#if DEBUG_UTOPIA >= DEBUG_BASIC
        m_log->debug("[lookupWayRadix] VA=", SimLog::hex(address), " app=", app_id, found ? " HIT way=" : " MISS way=", way_out);
#endif
        return found;
    }

    // ---- allocation --------------------------------------------------------

    // Place the page.  Returns (success, evicted, evicted virtual address).
    // A page already present is a hit (success, nothing changes); a full set
    // fails unless force_evict, which replaces the LRU entry and reports it.
    std::tuple<bool, bool, IntPtr> allocate(IntPtr address, int app_id, bool force_evict = false)
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        UInt64 vpn = address >> page_size_bits;
        UInt64 owner = app_id + 1;
        uint8_t fp = utopiaFingerprint(tag, fingerprint_bits);
        global_way_attempts++;

        UInt64 region_id = vpn >> RLE_REGION_BITS;
        bool use_region_pref = (page_size_bits == 12);
        int region_pref_way = -1;
        if (use_region_pref)
        {
            auto it = region_way_pref.find(region_id);
            if (it != region_way_pref.end())
                region_pref_way = it->second.way;
        }

        int used_way = -1;
        bool was_hit = false, used_free_way = false, success = false, evicted = false;
        bool success_from_region_pref = false;
        UInt64 evicted_tag = 0, evicted_owner = 0;

        // 1. the region's preferred way
        if (region_pref_way >= 0)
        {
            region_way_attempts++;
            if (sets[set_index].tryInsertAtWay(tag, owner, fp, region_pref_way, used_way, was_hit))
            {
                success = true;
                success_from_region_pref = true;
                if (!was_hit)
                {
                    used_free_way = true;
                    region_way_hits++;
                }
            }
        }
        // 2. the global preferred way
        if (!success && region_pref_way != global_way)
        {
            if (sets[set_index].tryInsertAtWay(tag, owner, fp, global_way, used_way, was_hit))
            {
                success = true;
                if (!was_hit)
                {
                    used_free_way = true;
                    global_way_hits++;
                }
            }
        }
        // 3. any free way, or the LRU victim when forced
        if (!success)
        {
            global_way_fallbacks++;
            RestSegSet::InsertResult r = sets[set_index].insert(tag, owner, fp, force_evict);
            success = r.success;
            evicted = r.evicted;
            evicted_tag = r.evicted_tag;
            evicted_owner = r.evicted_owner;
            was_hit = r.was_hit;
            used_free_way = r.used_free_way;
            used_way = r.used_way;
        }

        if (use_region_pref && !success_from_region_pref && region_pref_way >= 0)
        {
            auto &pref = region_way_pref[region_id];
            if (++pref.failures >= REGION_PREF_FAILURE_THRESHOLD)
            {
                pref.way = -1;
                pref.failures = 0;
                region_way_relearns++;
            }
        }
        if (!success)
            return std::make_tuple(false, false, (IntPtr)0);

        if (!was_hit)
        {
            allocations++;
            if (used_free_way && used_way >= 0 && used_way < (int)associativity && free_sets_per_way[used_way] > 0)
                free_sets_per_way[used_way]--;
            if (used_way >= 0)
                updateWayBitmap(set_index, used_way, false);
            if (use_region_pref)
                fillRegionPref(success_from_region_pref, region_id, used_way);
            maybeRotateGlobalWay();
        }

        if (evicted)
        {
            conflicts++;
            IntPtr evicted_addr = reconstructAddress(evicted_tag, set_index);
            radix_way_tables->clear_mapping(evicted_addr >> page_size_bits, (int)evicted_owner - 1);
            radix_way_tables->set_mapping(vpn, app_id, used_way);
            return std::make_tuple(true, true, evicted_addr);
        }
        if (!was_hit)
            radix_way_tables->set_mapping(vpn, app_id, used_way);
        return std::make_tuple(true, false, (IntPtr)0);
    }

    // ---- coalescing --------------------------------------------------------

    // Place a page in one given way (no fallback, no eviction); a page
    // already present is a hit.  With update_radix false the per-page way
    // table is left alone: the region's 2MB entry stands for it.
    bool tryInsertAtWay(IntPtr address, int app_id, int way, bool update_radix = true)
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        int used_way = -1;
        bool was_hit = false;
        if (!sets[set_index].tryInsertAtWay(tag, app_id + 1, utopiaFingerprint(tag, fingerprint_bits), way, used_way, was_hit))
            return false;
        if (!was_hit && used_way >= 0)
        {
            allocations++;
            updateWayBitmap(set_index, used_way, false);
            if (used_way < (int)associativity && free_sets_per_way[used_way] > 0)
                free_sets_per_way[used_way]--;
            if (update_radix)
                radix_way_tables->set_mapping(address >> page_size_bits, app_id, used_way);
        }
        return true;
    }

    // The lowest way free in every set of the 512 pages from start_vpn, or
    // -1: the AND of the sets' free-way bitmaps, stopping once it is empty
    int findCommonFreeWay(UInt64 start_vpn)
    {
        std::lock_guard<std::mutex> guard(lock);
        uint64_t candidates = (associativity >= 64) ? ~0ULL : ((1ULL << associativity) - 1);
        for (int i = 0; i < 512; i++)
        {
            UInt64 tag;
            UInt32 set_index;
            splitAddress((start_vpn + i) << 12, tag, set_index);
            candidates &= set_way_bitmap[set_index];
            if (candidates == 0)
                return -1;
        }
        return __builtin_ctzll(candidates);
    }

    void setCoalesced2MBWay(UInt64 region_start_vpn, int app_id, int way) { radix_way_tables_2mb->set_mapping(region_start_vpn >> 9, app_id, way); }
    void clearCoalesced2MBWay(UInt64 region_start_vpn, int app_id) { radix_way_tables_2mb->clear_mapping(region_start_vpn >> 9, app_id); }
    int lookupCoalesced2MBWay(UInt64 region_start_vpn, int app_id) const
    {
        coalesced_2mb_lookups++;
        int way_out = -1;
        if (radix_way_tables_2mb->lookup_way(region_start_vpn >> 9, app_id, way_out))
        {
            coalesced_2mb_hits++;
            return way_out;
        }
        return -1;
    }

    // The 2MB page a coalesced region is: the frame of the first of its 512
    // consecutive sets in the way (way-major layout, low index bits unfolded)
    IntPtr coalescedRegionPPN(IntPtr aligned_address, int way) const
    {
        UInt64 tag;
        UInt32 set_index;
        splitAddress(aligned_address, tag, set_index);
        return calculatePPNFromWay(set_index & ~(UInt32)511, way);
    }

    // The lines of a 2MB-table lookup, root first
    void calculateRadixLevelAddressesCoalesced(IntPtr address, int app_id, std::vector<std::pair<IntPtr, bool>> &level_addresses) const
    {
        level_addresses.clear();
        std::vector<std::tuple<int, IntPtr, bool>> traversal;
        radix_way_tables_2mb->getTraversalAddresses(address >> 21, app_id, traversal);
        for (const auto &entry : traversal)
            level_addresses.push_back(std::make_pair(std::get<1>(entry), std::get<2>(entry)));
    }
    UInt64 *getCoalesced2MBLookupsPtr() { return &coalesced_2mb_lookups; }
    UInt64 *getCoalesced2MBHitsPtr() { return &coalesced_2mb_hits; }

    // ---- translation -------------------------------------------------------

    // PPN (4KB units) of the page, or -1 when it is not here
    IntPtr calculatePhysicalAddress(IntPtr address, int app_id) const
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        int way = sets[set_index].findEntry(tag, app_id + 1);
        if (way < 0)
            return (IntPtr)-1;
        return calculatePPNFromWay(set_index, way);
    }

    // PPN (4KB units) of a way of the page's set, whether or not it holds it
    IntPtr calculatePPNFromWay(IntPtr address, int way) const
    {
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        return calculatePPNFromWay(set_index, way);
    }
    IntPtr calculatePPNFromWay(UInt32 set_index, int way) const
    {
        int factor = 1 << (page_size_bits - 12);
        return base_physical_address + ((IntPtr)way * num_sets + set_index) * factor;
    }

    // Full byte address of the access, were the page in a given way
    IntPtr calculatePhysicalAddressForWay(IntPtr address, int way) const
    {
        IntPtr page_offset = address & ((1ULL << page_size_bits) - 1);
        return (calculatePPNFromWay(address, way) << 12) | page_offset;
    }

    void touchWayLRU(IntPtr address, int way)
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        sets[set_index].touchEntry(way);
    }

    // ---- metadata addresses (for latency modelling) ------------------------

    // The core's tag-array line of the page's set (all ways read together)
    IntPtr calculateTagAddress(IntPtr address, int core_id) const
    {
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        if (core_id < 0 || core_id >= (int)tag_bases.size())
            return 0;
        return tag_bases[core_id] + (IntPtr)set_index * associativity * tag_entry_bytes;
    }

    // The core's fingerprint-array line of the page's set (one byte per way)
    IntPtr calculateFingerprintAddress(IntPtr address, int core_id) const
    {
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        if (core_id < 0 || core_id >= (int)fingerprint_bases.size())
            return 0;
        return fingerprint_bases[core_id] + (IntPtr)set_index * associativity;
    }

    // The cache lines a radix way-table lookup reads, root first
    void calculateRadixLevelAddresses(IntPtr address, int app_id, std::vector<std::pair<IntPtr, bool>> &level_addresses) const
    {
        level_addresses.clear();
        if (!radix_way_tables)
            return;
        std::vector<std::tuple<int, IntPtr, bool>> traversal;
        radix_way_tables->getTraversalAddresses(address >> page_size_bits, app_id, traversal);
        for (const auto &entry : traversal)
            level_addresses.push_back(std::make_pair(std::get<1>(entry), std::get<2>(entry)));
    }

    uint8_t computeLookupFingerprint(IntPtr address) const
    {
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        return utopiaFingerprint(tag, fingerprint_bits);
    }

    // The valid ways of the page's set whose fingerprint matches
    std::vector<int> findMatchingFingerprints(IntPtr address, uint8_t lookup_fingerprint) const
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 tag;
        UInt32 set_index;
        splitAddress(address, tag, set_index);
        std::vector<int> matching_ways;
        for (UInt32 way = 0; way < associativity; way++)
        {
            const RestSegEntry *entry = sets[set_index].getEntry(way);
            if (entry && entry->valid && entry->fingerprint == lookup_fingerprint)
                matching_ways.push_back(way);
        }
        return matching_ways;
    }

    // ---- accessors ---------------------------------------------------------

    int getId() const { return id; }
    int getPageSizeBits() const { return page_size_bits; }
    UInt32 getAssociativity() const { return associativity; }
    UInt32 getNumSets() const { return num_sets; }
    UInt64 getSizeBytes() const { return size_bytes; }
    UInt64 getSizeMB() const { return size_bytes / (1024 * 1024); }
    void setBase(IntPtr base) { base_physical_address = base; }
    IntPtr getBase() const { return base_physical_address; }
    void setMetadataBases(int core_id, IntPtr tag_base, IntPtr fp_base)
    {
        if (core_id >= 0 && core_id < (int)tag_bases.size())
        {
            tag_bases[core_id] = tag_base;
            fingerprint_bases[core_id] = fp_base;
        }
    }
    UInt64 getTagArraySize() const { return (UInt64)num_sets * associativity * tag_entry_bytes; }
    UInt64 getFingerprintArraySize() const { return (UInt64)num_sets * associativity; }
    bool isFingerprintEnabled() const { return fingerprint_enabled; }
    int getFingerprintBits() const { return fingerprint_bits; }
    int getRadixLevels() const { return radix_way_tables ? radix_way_tables->getNumLevels() : 0; }
    int getRadixLeafEntries() const { return radix_way_tables ? radix_way_tables->getLeafEntries() : 0; }
    UtopiaRadixWayTableManager *getRadixWayTableManager() { return radix_way_tables.get(); }

    UInt64 getAccesses() const { return accesses; }
    UInt64 getHits() const { return hits; }
    UInt64 getConflicts() const { return conflicts; }
    UInt64 getAllocations() const { return allocations; }
    UInt64 *getAccessesPtr() { return &accesses; }
    UInt64 *getHitsPtr() { return &hits; }
    UInt64 *getConflictsPtr() { return &conflicts; }
    UInt64 *getAllocationsPtr() { return &allocations; }
    UInt64 *getFingerprintChecksPtr() { return &fingerprint_checks; }
    UInt64 *getFingerprintMatchesPtr() { return &fingerprint_matches; }
    UInt64 *getFingerprintFalsePositivesPtr() { return &fingerprint_false_positives; }
    UInt64 *getFingerprintTruePositivesPtr() { return &fingerprint_true_positives; }
    UInt64 *getRegionWayAttemptsPtr() { return &region_way_attempts; }
    UInt64 *getRegionWayHitsPtr() { return &region_way_hits; }
    UInt64 *getRegionWayRelearnsPtr() { return &region_way_relearns; }
    UInt64 *getGlobalWayRotationsPtr() { return &global_way_rotations; }
    UInt64 *getRadixLookupsPtr() { return &radix_lookups; }
    UInt64 *getRadixHitsPtr() { return &radix_hits; }
    UInt64 *getRadixMissesPtr() { return &radix_misses; }
    int getGlobalWay() const { return global_way; }

    UInt64 countValidEntries() const
    {
        std::lock_guard<std::mutex> guard(lock);
        UInt64 count = 0;
        for (const auto &set : sets)
            count += set.countValid();
        return count;
    }
};

// ============================================================================
// The allocator
// ============================================================================

template <typename Policy>
class UtopiaAllocator : public PhysicalMemoryAllocator, protected Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType = Buddy<BuddyPolicy>;

public:
    struct Stats
    {
        UInt64 restseg_2mb_allocations = 0;
        UInt64 restseg_4kb_allocations = 0;
        UInt64 restseg_evictions = 0;
        UInt64 flexseg_thp_allocations = 0;   // 2MB pages (promoted regions)
        UInt64 flexseg_4kb_allocations = 0;   // 4KB pages, inside a reservation or from the buddy
        UInt64 flexseg_thp_reserved = 0;
        UInt64 flexseg_thp_promoted = 0;
        UInt64 flexseg_thp_demoted = 0;
        UInt64 total_allocations = 0;
        UInt64 migrations = 0;
        UInt64 restseg_allocations = 0;       // sum of the two RestSeg counts
        UInt64 flexseg_allocations = 0;       // sum of the two FlexSeg counts
        UInt64 coalescing_promotions = 0;     // 2MB regions placed whole in one way (utopia_hash_coalesce.h)
        UInt64 coalescing_hits = 0;           // faults in a region coalesced already
    };

    // Cost-based Top-K migration: the MMU scores each 4KB FlexSeg page by what
    // its walks cost (a saturating counter bumped per walk by a base amount
    // plus the DRAM accesses the walk made), keeps the K most expensive in a
    // small table, and migrates one into the 4KB RestSeg once it has stayed
    // there (sticky), is above the threshold, is not cooling down from a
    // previous migration, and a migration credit is available.  Scores and
    // stickiness decay every ranking epoch; credits refill every credit epoch.
    enum class MigrationPolicyType { None = 0, CostTopK = 1 };

    struct CostTopKEntry
    {
        bool valid = false;
        UInt64 vpn = 0;
        UInt16 score_snapshot = 0;
        UInt8 sticky = 0;
        UInt8 cooldown = 0;
    };
    struct CostTopKConfig
    {
        bool enabled = false;
        int page_size_bits = 12;
        int score_bits = 10;
        int ptw_base_inc = 1;
        int dram_inc_cap = 3;
        int topk_entries = 16;
        int sticky_threshold = 2;
        int cooldown_epochs = 2;
        UInt64 rank_epoch_translations = 1024;
        UInt64 credit_epoch_translations = 8192;
        int credits_max = 4;
        int credits_refill = 1;
        int migrate_threshold = 32;
        int post_migrate_score_shift = 2;
    };
    struct CostTopKState
    {
        std::unordered_map<UInt64, UInt16> vpn_cost_score;
        std::vector<CostTopKEntry> topk_table;
        UInt16 topk_min_score = 0;
        int topk_min_idx = -1;
        UInt64 translation_counter = 0;
        UInt64 rank_epoch_counter = 0;
        UInt64 credit_epoch_counter = 0;
        int credits = 0;
        UInt64 score_updates = 0;
        UInt64 topk_inserts = 0;
        UInt64 topk_replacements = 0;
        UInt64 migration_attempts = 0;
        UInt64 migrations_issued = 0;
        UInt64 gated_credits = 0;
        UInt64 gated_threshold = 0;
        UInt64 gated_sticky = 0;
        UInt64 gated_cooldown = 0;
        IntPtr last_migration_ppn = 0;
    };

protected:
    Stats stats;
    std::vector<std::unique_ptr<RestSeg>> restsegs;
    BuddyType *buddy_allocator;
    int num_restsegs;
    int restseg_2mb_index = -1;
    int restseg_4kb_index = -1;
    double thp_promotion_threshold;

    // FlexSeg THP reservations: 2MB region index -> (base PPN, which 4KB
    // pages are in use, promoted), as reserve_thp.h keeps them
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>> thp_regions;

    MigrationPolicyType migration_policy_type = MigrationPolicyType::None;

    // What the last allocate() did, for the fault handler and the MMU
    bool last_in_restseg = false;
    bool last_caused_eviction = false;
    IntPtr last_evicted_address = 0;
    int last_evicted_page_size_bits = 12;

    CostTopKConfig cost_topk_config;
    CostTopKState cost_topk_state;

    // A kernel-region allocation rounded up to whole frames, as a byte address
    IntPtr kernelBytes(UInt64 bytes)
    {
        UInt64 rounded = (bytes + 4095) & ~(UInt64)4095;
        return (IntPtr)this->handle_page_table_allocations(rounded) * 4096;
    }

public:
    Stats &getStats() { return stats; }
    const Stats &getStats() const { return stats; }

    UtopiaAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
    {
        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
                      "BuddyPolicyFor<Policy> is incomplete. Include buddy_policy.h before utopia.h.");

        num_restsegs = Policy::read_config_int("perf_model/utopia/restsegs");
        // Percent of a 2MB region's pages that promotes it; 0 promotes on the first touch
        thp_promotion_threshold = Policy::read_config_int("perf_model/utopia/thp_promotion_threshold") / 100.0;
        if (thp_promotion_threshold < 0.0 || thp_promotion_threshold > 1.0)
            thp_promotion_threshold = 0.75;
        int num_cores = Policy::get_num_cores();

        // One buddy for every FlexSeg allocation, 4KB and 2MB alike.  It starts
        // after the kernel region, from which the RestSegs are carved below.
        buddy_allocator = new BuddyType(memory_size, max_order, kernel_size, frag_type);

        int fingerprint_bits = Policy::read_config_int_default("perf_model/utopia/fingerprint_bits", 4);
        if (fingerprint_bits < 1 || fingerprint_bits > 8)
            fingerprint_bits = 4;
        bool fingerprint_enabled = Policy::read_config_int_default("perf_model/utopia/fingerprint_enabled", 1) != 0;

        for (int i = 0; i < num_restsegs; i++)
        {
            int rs_size = Policy::read_config_int_array("perf_model/utopia/restseg/size", i);
            int rs_page_size = Policy::read_config_int_array("perf_model/utopia/restseg/page_size", i);
            int rs_assoc = Policy::read_config_int_array("perf_model/utopia/restseg/assoc", i);
            auto rs = std::make_unique<RestSeg>(i + 1, rs_size, rs_page_size, rs_assoc, num_cores, fingerprint_bits, fingerprint_enabled);

            // The segment's frames come from the kernel region, aligned to its
            // page size or to 2MB, whichever is larger: 512 consecutive
            // frames of a 4KB RestSeg are a 2MB page only from a 2MB-aligned
            // base (the coalescing of utopia_hash_coalesce.h).  One extra
            // alignment unit is taken so that the aligned segment still ends
            // inside what was reserved.
            UInt64 align_bytes = std::max(1ULL << rs_page_size, 2ULL << 20);
            UInt64 rs_bytes = rs->getSizeMB() * 1024ULL * 1024ULL;
            UInt64 base_ppn = this->handle_page_table_allocations(rs_bytes + align_bytes);
            UInt64 pages_per_page = align_bytes / 4096;
            UInt64 aligned_base_ppn = ((base_ppn + pages_per_page - 1) / pages_per_page) * pages_per_page;
            rs->setBase(aligned_base_ppn);

            // Each core's FPA and TAR, also from the kernel region
            for (int c = 0; c < num_cores; c++)
            {
                IntPtr tag_addr = kernelBytes(rs->getTagArraySize());
                IntPtr fp_addr = kernelBytes(rs->getFingerprintArraySize());
                rs->setMetadataBases(c, tag_addr, fp_addr);
            }
            rs->setRadixAllocator([this](size_t size) -> IntPtr { return kernelBytes(size); });

            if (rs_page_size == 21)
                restseg_2mb_index = i;
            else if (rs_page_size == 12)
                restseg_4kb_index = i;
            restsegs.push_back(std::move(rs));
        }

        cost_topk_config.enabled = Policy::read_config_int_default("perf_model/utopia/migration_cost_topk/enabled", 0) != 0;
        migration_policy_type = cost_topk_config.enabled ? MigrationPolicyType::CostTopK : MigrationPolicyType::None;
        if (cost_topk_config.enabled)
        {
            const char *p = "perf_model/utopia/migration_cost_topk/";
            auto rd = [&](const char *key, int def) { return Policy::read_config_int_default((std::string(p) + key).c_str(), def); };
            cost_topk_config.page_size_bits = rd("page_size_bits", 12);
            cost_topk_config.score_bits = rd("score_bits", 10);
            cost_topk_config.ptw_base_inc = rd("ptw_base_increment", 1);
            cost_topk_config.dram_inc_cap = rd("dram_weighted_increment_cap", 3);
            cost_topk_config.topk_entries = rd("topk_entries", 16);
            cost_topk_config.sticky_threshold = rd("sticky_threshold", 2);
            cost_topk_config.cooldown_epochs = rd("cooldown_epochs", 2);
            cost_topk_config.rank_epoch_translations = rd("ranking_epoch_translations", 1024);
            cost_topk_config.credit_epoch_translations = rd("credit_epoch_translations", 8192);
            cost_topk_config.credits_max = rd("credits_max", 4);
            cost_topk_config.credits_refill = rd("credits_refill", 1);
            cost_topk_config.migrate_threshold = rd("migrate_threshold", 32);
            cost_topk_config.post_migrate_score_shift = rd("post_migrate_score_shift", 2);
            cost_topk_state.topk_table.resize(cost_topk_config.topk_entries);
            cost_topk_state.credits = cost_topk_config.credits_max;
        }

        Policy::on_init(name, memory_size, kernel_size, this);
        this->log("Utopia initialised: RestSeg-2MB idx=", restseg_2mb_index, " RestSeg-4KB idx=", restseg_4kb_index,
                  " THP threshold=", thp_promotion_threshold, " cost_topk=", cost_topk_config.enabled);
    }

    ~UtopiaAllocator() { delete buddy_allocator; }

    // ---- FlexSeg THP (after reserve_thp.h) ---------------------------------
protected:
    // (PPN, promoted): the page's frame inside its 2MB reservation, reserving
    // the region on its first touch; promoted once the threshold is reached,
    // after which the region's base PPN is returned as a 2MB page.  -1 when no
    // 2MB region can be reserved.
    std::pair<UInt64, bool> tryAllocateTHP(UInt64 address, UInt64 core_id)
    {
        UInt64 region_2mb = address >> 21;
        int offset_4kb = (address >> 12) & 0x1FF;
        if (thp_regions.find(region_2mb) == thp_regions.end())
        {
            auto reserved = buddy_allocator->reserve_2mb_page(address, core_id);
            if (std::get<0>(reserved) == (UInt64)-1)
                return std::make_pair((UInt64)-1, false);
            thp_regions[region_2mb] = std::make_tuple(std::get<0>(reserved), std::bitset<512>(), false);
            stats.flexseg_thp_reserved++;
            this->log_trace("Reserved 2MB region ", region_2mb, " base_ppn=", std::get<0>(reserved));
        }
        auto &[base_ppn, bits, promoted] = thp_regions[region_2mb];
        if (promoted)
            return std::make_pair(base_ppn, true);
        if (bits.test(offset_4kb))
            return std::make_pair(base_ppn + offset_4kb, false);   // same VA, same PPN
        bits.set(offset_4kb);
        double utilization = (double)bits.count() / 512.0;
        if (utilization >= thp_promotion_threshold)
        {
            promoted = true;
            stats.flexseg_thp_promoted++;
            this->log_debug("THP region promoted: ", region_2mb, " utilization=", utilization);
            return std::make_pair(base_ppn, true);
        }
        return std::make_pair(base_ppn + offset_4kb, false);
    }

    // Free the unused pages of the least used reservation back to the buddy
    bool demoteTHPRegion()
    {
        std::vector<std::pair<UInt64, double>> utilizations;
        for (auto &[region_idx, region] : thp_regions)
        {
            auto &[base_ppn, bits, promoted] = region;
            if (promoted)
                continue;
            utilizations.push_back({region_idx, (double)bits.count() / 512.0});
        }
        if (utilizations.empty())
            return false;
        std::sort(utilizations.begin(), utilizations.end(), [](auto &a, auto &b) { return a.second < b.second; });
        UInt64 victim = utilizations[0].first;
        auto &[base_ppn, bits, promoted] = thp_regions[victim];
        for (int i = 0; i < 512; i++)
            if (!bits[i])
                buddy_allocator->free(base_ppn + i, base_ppn + i + 1);
        stats.flexseg_thp_demoted++;
        thp_regions.erase(victim);
        this->log_debug("Demoted THP region ", victim, " utilization=", utilizations[0].second);
        return true;
    }

    bool isInRestSeg(IntPtr address, int app_id)
    {
        for (int i = 0; i < num_restsegs; i++)
            if (restsegs[i]->inRestSeg(address, app_id, false))
                return true;
        return false;
    }

public:
    // Base PPN of the page's reserved 2MB region, or -1 (the SpecTLB engine's query)
    IntPtr isLargePageReserved(IntPtr address)
    {
        auto it = thp_regions.find(address >> 21);
        return (it != thp_regions.end()) ? (IntPtr)std::get<0>(it->second) : (IntPtr)-1;
    }

    // ---- allocation --------------------------------------------------------

    // (PPN in 4KB units, page size in bits).  core_id is the faulting core;
    // RestSeg ownership is by the application running on it, which is what
    // the MMU's lookups are keyed by.  Page-table frames always come from the
    // buddy, instruction pages from the reserved instruction area.
    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1,
                                       bool is_pagetable_allocation = false,
                                       bool is_instruction_allocation = false) override
    {
        stats.total_allocations++;
        last_in_restseg = false;
        last_caused_eviction = false;
        last_evicted_address = 0;

        if (is_pagetable_allocation)
        {
            UInt64 page = buddy_allocator->allocate(size, address, core_id);
            stats.flexseg_4kb_allocations++;
            stats.flexseg_allocations++;
            return std::make_pair(page, 12ULL);
        }
        if (is_instruction_allocation)
            return this->allocateInstruction(size);

        int app_id = Policy::app_id_for_core(core_id);

        // 1. RestSeg-2MB
        if (restseg_2mb_index >= 0)
        {
            auto [success, evicted, evicted_addr] = restsegs[restseg_2mb_index]->allocate(address, app_id, false);
            if (success)
            {
                stats.restseg_2mb_allocations++;
                stats.restseg_allocations++;
                last_in_restseg = true;
                IntPtr ppn = restsegs[restseg_2mb_index]->calculatePhysicalAddress(address, app_id);
                this->log_debug("[L1] RestSeg-2MB: VA=", SimLog::hex(address), " -> PPN=", ppn);
                return std::make_pair((UInt64)ppn, 21ULL);
            }
        }
        // 2. FlexSeg-THP: a reservation wins over RestSeg-4KB even while the
        //    region is still mapped 4KB by 4KB
        {
            auto [phys, promoted] = tryAllocateTHP(address, core_id);
            if (phys != (UInt64)-1)
            {
                if (promoted)
                {
                    stats.flexseg_thp_allocations++;
                    stats.flexseg_allocations++;
                    this->log_debug("[L2] FlexSeg-THP (2MB): VA=", SimLog::hex(address), " -> PPN=", phys);
                    return std::make_pair(phys, 21ULL);
                }
                stats.flexseg_4kb_allocations++;
                stats.flexseg_allocations++;
                this->log_debug("[L2] FlexSeg-THP (4KB in reservation): VA=", SimLog::hex(address), " -> PPN=", phys);
                return std::make_pair(phys, 12ULL);
            }
        }
        // 3. RestSeg-4KB
        if (restseg_4kb_index >= 0)
        {
            auto [success, evicted, evicted_addr] = restsegs[restseg_4kb_index]->allocate(address, app_id, false);
            if (success)
            {
                stats.restseg_4kb_allocations++;
                stats.restseg_allocations++;
                last_in_restseg = true;
                IntPtr ppn = restsegs[restseg_4kb_index]->calculatePhysicalAddress(address, app_id);
                this->log_debug("[L3] RestSeg-4KB: VA=", SimLog::hex(address), " -> PPN=", ppn);
                return std::make_pair((UInt64)ppn, 12ULL);
            }
        }
        // 4. FlexSeg-4KB, demoting a reservation when the buddy is empty
        UInt64 page = buddy_allocator->allocate(size, address, core_id);
        if (page == (UInt64)-1 && demoteTHPRegion())
            page = buddy_allocator->allocate(size, address, core_id);
        if (page != (UInt64)-1)
        {
            stats.flexseg_4kb_allocations++;
            stats.flexseg_allocations++;
            this->log_debug("[L4] FlexSeg-4KB: VA=", SimLog::hex(address), " -> PPN=", page);
            return std::make_pair(page, 12ULL);
        }
        this->log("Out of memory: VA=", SimLog::hex(address), " RS-2MB=", stats.restseg_2mb_allocations,
                  " FS-THP=", stats.flexseg_thp_allocations, " RS-4KB=", stats.restseg_4kb_allocations,
                  " FS-4KB=", stats.flexseg_4kb_allocations);
        return std::make_pair((UInt64)-1, 12ULL);
    }

    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id) override { return std::vector<Range>(); }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1) override
    {
        return buddy_allocator->allocate(bytes, address, core_id);
    }

    void deallocate(UInt64 address, UInt64 core_id = -1) override {}   // as the other allocators: not implemented

    void fragment_memory(double target) override { buddy_allocator->fragmentMemory(target); }

    // ---- RestSeg access ----------------------------------------------------

    RestSeg *getRestSeg2MB() { return (restseg_2mb_index >= 0) ? restsegs[restseg_2mb_index].get() : nullptr; }
    RestSeg *getRestSeg4KB() { return (restseg_4kb_index >= 0) ? restsegs[restseg_4kb_index].get() : nullptr; }
    RestSeg *getRestSeg(int index) { return (index >= 0 && index < num_restsegs) ? restsegs[index].get() : nullptr; }
    int getNumRestSegs() const { return num_restsegs; }
    int getRestSeg2MBIndex() const { return restseg_2mb_index; }
    int getRestSeg4KBIndex() const { return restseg_4kb_index; }
    double getTHPPromotionThreshold() const { return thp_promotion_threshold; }

    bool getLastAllocatedInRestSeg() const { return last_in_restseg; }
    bool didLastCauseEviction() const { return last_caused_eviction; }
    IntPtr getLastEvictedAddress() const { return last_evicted_address; }
    int getLastEvictedPageSizeBits() const { return last_evicted_page_size_bits; }
    bool checkIsInRestSeg(IntPtr address, int app_id) { return isInRestSeg(address, app_id); }

    // ---- migration ---------------------------------------------------------

    // Promote a FlexSeg page into the RestSeg of the given page size, evicting
    // that set's LRU page if the set is full (the evicted page is left to
    // fault back in: its mapping is gone from the RestSeg and it had no page
    // table entry).  The old frame goes back to the buddy and the page's
    // table entry is deleted.  Returns the new PPN, or 0 when nothing moved:
    // the page is in a RestSeg already, sits in a THP reservation, or no
    // RestSeg of that page size has it.  The caller shoots down the TLBs.
    IntPtr migratePage(IntPtr address, IntPtr old_ppn, int target_page_size_bits, int app_id)
    {
        if (isInRestSeg(address, app_id))
            return 0;
        if (thp_regions.find(address >> 21) != thp_regions.end())
            return 0;
        last_caused_eviction = false;
        last_evicted_address = 0;
        last_evicted_page_size_bits = 12;
        for (int i = 0; i < num_restsegs; i++)
        {
            if (restsegs[i]->getPageSizeBits() != target_page_size_bits)
                continue;
            auto [success, evicted, evicted_addr] = restsegs[i]->allocate(address, app_id, true);
            if (!success)
                continue;
            stats.migrations++;
            if (evicted)
            {
                stats.restseg_evictions++;
                last_caused_eviction = true;
                last_evicted_address = evicted_addr;
                last_evicted_page_size_bits = restsegs[i]->getPageSizeBits();
            }
            buddy_allocator->free(old_ppn, old_ppn);
            Policy::deletePageTableEntry(address, app_id);
            IntPtr new_ppn = restsegs[i]->calculatePhysicalAddress(address, app_id);
            this->log_debug("Migrated VA=", SimLog::hex(address), " old_ppn=", old_ppn, " -> new_ppn=", new_ppn,
                            evicted ? " (evicted a page)" : "");
            return new_ppn;
        }
        return 0;
    }

    MigrationPolicyType getMigrationPolicyType() const { return migration_policy_type; }

    // ---- cost-based Top-K migration ----------------------------------------

    UInt16 getCostScore(UInt64 vpn) const
    {
        auto it = cost_topk_state.vpn_cost_score.find(vpn);
        return (it != cost_topk_state.vpn_cost_score.end()) ? it->second : 0;
    }
    void setCostScore(UInt64 vpn, UInt16 score)
    {
        UInt16 max_score = (1 << cost_topk_config.score_bits) - 1;
        cost_topk_state.vpn_cost_score[vpn] = std::min(score, max_score);
    }
    UInt16 bumpCostScore(UInt64 vpn, UInt16 inc)
    {
        UInt16 max_score = (1 << cost_topk_config.score_bits) - 1;
        UInt16 new_score = std::min((UInt16)(getCostScore(vpn) + inc), max_score);
        cost_topk_state.vpn_cost_score[vpn] = new_score;
        cost_topk_state.score_updates++;
        return new_score;
    }

    void updateCostTopK(UInt64 vpn, UInt16 score, bool count_stats)
    {
        if (!cost_topk_config.enabled)
            return;
        auto &table = cost_topk_state.topk_table;
        int k = cost_topk_config.topk_entries;
        for (int i = 0; i < k; i++)
            if (table[i].valid && table[i].vpn == vpn)
            {
                table[i].score_snapshot = score;
                if (table[i].sticky < 3) table[i].sticky++;
                recomputeTopKMin();
                return;
            }
        for (int i = 0; i < k; i++)
            if (!table[i].valid)
            {
                table[i] = CostTopKEntry{true, vpn, score, 1, 0};
                if (count_stats) cost_topk_state.topk_inserts++;
                recomputeTopKMin();
                return;
            }
        if (score > cost_topk_state.topk_min_score && cost_topk_state.topk_min_idx >= 0)
        {
            table[cost_topk_state.topk_min_idx] = CostTopKEntry{true, vpn, score, 1, 0};
            if (count_stats) cost_topk_state.topk_replacements++;
            recomputeTopKMin();
        }
    }

    // Called once per walk-serviced translation: ranking decay and credit refill
    void processEpochs(bool count_stats)
    {
        if (!cost_topk_config.enabled)
            return;
        cost_topk_state.translation_counter++;
        if (++cost_topk_state.rank_epoch_counter >= cost_topk_config.rank_epoch_translations)
        {
            cost_topk_state.rank_epoch_counter = 0;
            for (auto &entry : cost_topk_state.topk_table)
                if (entry.valid)
                {
                    entry.score_snapshot >>= 1;
                    if (entry.sticky > 0) entry.sticky--;
                    if (entry.cooldown > 0) entry.cooldown--;
                }
            recomputeTopKMin();
        }
        if (++cost_topk_state.credit_epoch_counter >= cost_topk_config.credit_epoch_translations)
        {
            cost_topk_state.credit_epoch_counter = 0;
            cost_topk_state.credits = std::min(cost_topk_state.credits + cost_topk_config.credits_refill, cost_topk_config.credits_max);
        }
    }

    // (migrated, new PPN) for a 4KB FlexSeg page the MMU just walked
    std::pair<bool, IntPtr> considerCostBasedMigration(IntPtr va, IntPtr current_ppn, int page_size_bits,
                                                       int app_id, UInt64 vpn, bool count_stats)
    {
        if (!cost_topk_config.enabled || page_size_bits != cost_topk_config.page_size_bits)
            return {false, 0};
        if (count_stats) cost_topk_state.migration_attempts++;
        int found_idx = -1;
        for (int i = 0; i < cost_topk_config.topk_entries; i++)
            if (cost_topk_state.topk_table[i].valid && cost_topk_state.topk_table[i].vpn == vpn)
            {
                found_idx = i;
                break;
            }
        if (found_idx < 0)
            return {false, 0};
        auto &entry = cost_topk_state.topk_table[found_idx];
        if (entry.score_snapshot < (UInt16)cost_topk_config.migrate_threshold)
        {
            if (count_stats) cost_topk_state.gated_threshold++;
            return {false, 0};
        }
        if (entry.sticky < (UInt8)cost_topk_config.sticky_threshold)
        {
            if (count_stats) cost_topk_state.gated_sticky++;
            return {false, 0};
        }
        if (entry.cooldown > 0)
        {
            if (count_stats) cost_topk_state.gated_cooldown++;
            return {false, 0};
        }
        if (cost_topk_state.credits <= 0)
        {
            if (count_stats) cost_topk_state.gated_credits++;
            return {false, 0};
        }
        int target_page_size_bits;
        if (restseg_4kb_index >= 0)
            target_page_size_bits = restsegs[restseg_4kb_index]->getPageSizeBits();
        else if (restseg_2mb_index >= 0)
            target_page_size_bits = 21;
        else
            return {false, 0};
        IntPtr new_ppn = migratePage(va, current_ppn, target_page_size_bits, app_id);
        if (new_ppn == 0)
            return {false, 0};
        cost_topk_state.credits--;
        entry.cooldown = cost_topk_config.cooldown_epochs;
        UInt16 new_score = getCostScore(vpn) >> cost_topk_config.post_migrate_score_shift;
        setCostScore(vpn, new_score);
        entry.score_snapshot = new_score;
        cost_topk_state.last_migration_ppn = new_ppn;
        if (count_stats) cost_topk_state.migrations_issued++;
        return {true, new_ppn};
    }

    IntPtr getLastMigrationPPN() const { return cost_topk_state.last_migration_ppn; }
    bool isCostTopKEnabled() const { return cost_topk_config.enabled; }
    const CostTopKConfig &getCostTopKConfig() const { return cost_topk_config; }
    CostTopKState &getCostTopKState() { return cost_topk_state; }

private:
    void recomputeTopKMin()
    {
        cost_topk_state.topk_min_score = UINT16_MAX;
        cost_topk_state.topk_min_idx = -1;
        for (int i = 0; i < cost_topk_config.topk_entries; i++)
        {
            const auto &e = cost_topk_state.topk_table[i];
            if (e.valid && e.score_snapshot < cost_topk_state.topk_min_score)
            {
                cost_topk_state.topk_min_score = e.score_snapshot;
                cost_topk_state.topk_min_idx = i;
            }
        }
        if (cost_topk_state.topk_min_idx < 0)
            cost_topk_state.topk_min_score = 0;
    }

public:
    BuddyType *getBuddyAllocator() { return buddy_allocator; }
};

// The Sniper instantiation, for the MMU and the fault handler
#include "memory_management/policies/utopia_policy.h"
#include "memory_management/policies/buddy_policy.h"
using Utopia = UtopiaAllocator<Sniper::Utopia::MetricsPolicy>;
