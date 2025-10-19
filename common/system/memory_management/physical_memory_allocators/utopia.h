#ifndef __UTOPIA_H
#define __UTOPIA_H

#include "stats.h"
#include "config.hpp"
#include "simulator.h"
#include <cmath>
#include <iostream>
#include <utility>
#include "cache.h"
#include <vector>
#include "stats.h"
#include "utils.h"
#include "cache_set.h"
#include "hash_map_set.h"
#include "physical_memory_allocator.h"
#include "buddy_allocator.h"


using namespace std;


class RestSeg
{

        // @kanellok: Add Replacement Policy in a RestSeg
private:
        int id;
        long long int size;
        int page_size; // RestSeg can be 4KB, 2MB, 1GB
        uint assoc;
        String hash;
        String repl;
        int num_sets;
        int TAR_size;
        int SF_size;
        int filter_size;
        int tag_size;

        IntPtr m_RestSeg_base;

        UInt64 m_RestSeg_conflicts,
            m_RestSeg_accesses, m_RestSeg_hits, m_allocations, m_pagefaults;

        std::unordered_map<IntPtr, bool> vpn_container; // container to check if vpn is inside RestSeg

        IntPtr RestSeg_tags_base; // Used to perform mem access
        IntPtr RestSeg_permissions_base;

        std::vector<IntPtr> permissions;
        std::vector<IntPtr> tags;

        int *RestSeg_allocation_heatmap;

        std::ofstream log_file;
        std::string log_file_name;

public:
        std::vector<int> utilization;
        Cache *m_RestSeg_cache;
        Lock *RestSeg_lock; // We need to lock RestSeg every time we read/modify

        RestSeg(int id, int size, int page_size, int assoc, String repl, String hash_function);
        ~RestSeg();


        bool inRestSeg(IntPtr address, bool count, SubsecondTime now, int core_id);
        std::tuple<bool,bool,IntPtr> allocate(IntPtr address, SubsecondTime now, int core_id, bool forced=false);
        bool permission_filter(IntPtr address, int core_id);

        int getSize() { return size; }
        std::unordered_map<IntPtr, bool> getVpnContainer() { return vpn_container; }
        int getPageSize() { return page_size; }
        int getAssoc() { return assoc; }
        std::vector<IntPtr> getPermissionsVector() { return permissions; }
        IntPtr getPermissions() { return RestSeg_permissions_base; }
        IntPtr getTags() { return RestSeg_tags_base; }

        IntPtr calculate_permission_address(IntPtr address, int core_id);
        IntPtr calculate_tag_address(IntPtr address, int core_id);
        IntPtr calculate_physical_address(IntPtr address, int core_id);
        void track_utilization();

        void set_base(IntPtr base) { m_RestSeg_base = base; }
};

class Utopia : public PhysicalMemoryAllocator
{
private:
        std::vector<RestSeg *> RestSeg_vector;

public:
        int RestSegs;

        enum utopia_heuristic
        {
                none = 0,
                tlb,
                pte,
                pf
        };

        utopia_heuristic heur_type_primary;
        utopia_heuristic heur_type_secondary;

        int pte_eviction_thr;
        int tlb_eviction_thr;

        Buddy *buddy_allocator;
        bool m_last_allocated_in_restseg;
        bool m_last_allocated_in_restseg_caused_eviction;
        IntPtr m_last_allocated_in_restseg_evicted_address;

        std::ofstream log_file;
        std::string log_file_name;

        Utopia(String name, int memory_size, int max_order, int kernel_size, String frag_type);
        ~Utopia();
        
        std::pair<UInt64,UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
        std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id);
        void deallocate(UInt64 address, UInt64 core_id);
        void fragment_memory();


        std::vector<RestSeg *> getRestSegVector() { return RestSeg_vector; }
        RestSeg *getRestSeg(int index) { return RestSeg_vector[index]; }
        utopia_heuristic getHeurPrimary() { return heur_type_primary; }
        utopia_heuristic getHeurSecondary() { return heur_type_secondary; }
        int getPTEthr() { return pte_eviction_thr; }
        int getTLBthr() { return tlb_eviction_thr; }
        
        bool getLastAllocatedInRestSeg() { return m_last_allocated_in_restseg; }
        IntPtr getLastAllocatedInRestSegEvictedAddress() { return m_last_allocated_in_restseg_evicted_address; }

        IntPtr migratePage(IntPtr address, IntPtr ppn, int page_size, int app_id);

        static const UInt64 HASH_PRIME = 124183;
};

#endif
