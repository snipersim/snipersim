#pragma once
#include "rangetable.h"
#include "rangetable_btree.h"

namespace ParametricDramDirectoryMSI
{
    class RangeTableFactory
    {
    public:
        static RangeTable *createRangeTable(String type, String name, int app_id)
        {
            if (type == "btree")
            {
                int node_size = Sim()->getCfg()->getInt("perf_model/" + name + "/node_size");
                return new RangeTableBtree(name, node_size, app_id);
            }
            else {
                return NULL; // Unsupported range table type
            }
        }
    };
}
