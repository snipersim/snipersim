
#pragma once
#include <iostream>
#include <vector>
#include <utility>
#include "core.h"
#include "fixed_types.h"
#include "stats.h"
#include "simulator.h"
#include <tuple>

using namespace std;
namespace ParametricDramDirectoryMSI
{

    struct RangeEntry
    {
        IntPtr offset;
    };

    class TreeNode
    {

     public:
        
        std::pair<uint64_t, uint64_t> *keys;
        RangeEntry *values;
        int t;
        TreeNode **C;
        int n;
        bool leaf;
        IntPtr emulated_ppn;
        TreeNode(int temp, bool bool_leaf);

        void insertNonFull(std::pair<uint64_t, uint64_t> k, RangeEntry value);
        void splitChild(int i, TreeNode *y);
        void traverse();

        std::pair<TreeNode*,int> search(IntPtr address, std::vector<IntPtr> &accessedAddresses);

        friend class RangeTableBtree;
    };

    class RangeTable
    {
    private:
        String name;
        struct Stats
        {
            uint64_t range_walks;
            uint64_t accesses;

        } rt_stats;

    public:
        RangeTable(String name, int app_id)
        {
            this->name = name;
            bzero(&rt_stats, sizeof(rt_stats));
            registerStatsMetric(name, app_id, "range_walks", &rt_stats.range_walks);
            registerStatsMetric(name, app_id, "accesses", &rt_stats.accesses);
        }
        virtual void insert(std::pair<uint64_t, uint64_t> key, RangeEntry value) = 0;
        virtual std::tuple<TreeNode *, int, std::vector<IntPtr>> lookup(uint64_t address) = 0;
        ~RangeTable(){};
    };

}
