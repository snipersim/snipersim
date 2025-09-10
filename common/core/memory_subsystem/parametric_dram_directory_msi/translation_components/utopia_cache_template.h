#ifndef UTOPIA_PERM_H
#define UTOPIA_PERM_H

#include "fixed_types.h"
#include "cache.h"
#include <util.h>
#include <unordered_map>
#include "lock.h"
#include <vector>

namespace ParametricDramDirectoryMSI
{
    class UtopiaCache
    {
    private:
        UInt64 m_access, m_miss;
        core_id_t m_core_id;

        Cache m_cache;
        CacheCntlr *m_next_level;

    public:
        enum where_t
        {
            HIT = 0,
            MISS
        };

        ComponentLatency access_latency;
        ComponentLatency miss_latency;

        UtopiaCache(String name, String cfgname, core_id_t core_id, UInt32 page_size, UInt32 num_entries, UInt32 associativity, ComponentLatency access_latency, ComponentLatency miss_latency);
        UtopiaCache::where_t lookup(IntPtr address, SubsecondTime now, bool allocate_on_miss, bool count);
        void allocate(IntPtr address, SubsecondTime now, Cache *Utopia_cache);
        void setNextLevel(CacheCntlr *next_level) { m_next_level = next_level; };
    };
}

#endif // ULB_H
