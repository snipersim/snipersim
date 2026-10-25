#pragma once
#include <iostream>
#include <vector>
#include <utility>
#include <tuple>
#include <fstream>

#include "core.h"
#include "fixed_types.h"
#include "rangetable.h"


using namespace std;

namespace ParametricDramDirectoryMSI
{

    class RangeTableBtree : public RangeTable
    {
        TreeNode *root;
        int t;
        std::vector<IntPtr> accessedAddresses;

        std::string log_file_name;
        std::ofstream log_file;

    public:
        RangeTableBtree(String name, int temp, int app_id);

        ~RangeTableBtree()
        {
            if (root != NULL)
                delete root;
            log_file.close();
        }

        void traverse()
        {
            if (root != NULL)
                root->traverse();
        }

        std::tuple<TreeNode *, int, std::vector<IntPtr>> lookup(IntPtr address)
        {
            accessedAddresses = std::vector<IntPtr>();
            if (root == NULL){
                return std::make_tuple(root,0, accessedAddresses);
            }
            else
            {
                auto result = root->search(address, accessedAddresses);
                return std::make_tuple(get<0>(result),get<1>(result),accessedAddresses);
            }
        }

        void insert(std::pair<uint64_t, uint64_t> key, RangeEntry value);
    };

}
