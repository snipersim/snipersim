#ifndef UTOPIA_CACHE_TEMPLATE_H
#define UTOPIA_CACHE_TEMPLATE_H

#include "fixed_types.h"
#include "cache.h"
#include <util.h>
#include <unordered_set>

namespace ParametricDramDirectoryMSI
{
  /**
   * @brief A small per-core cache of RestSeg metadata lines.
   *
   * The Utopia MMU keeps one for the fingerprint array (fpacache) and one
   * for the tag array (tarcache): a lookup that hits saves the trip to the
   * cache hierarchy for that line.  The access and miss penalties are read
   * from the configuration but, as upstream, never charged: the arrays are
   * read in parallel with the TLBs and their configured penalties are zero.
   */
  class UtopiaCache
  {
  private:
    UInt64 m_access, m_miss;
    UInt64 m_unique_cache_lines;   // distinct 64-byte lines ever looked up
    core_id_t m_core_id;
    ComponentLatency access_latency;
    ComponentLatency miss_latency;

    Cache m_cache;
    std::unordered_set<IntPtr> m_unique_lines_set;

  public:
    enum where_t
    {
      HIT = 0,
      MISS
    };

    UtopiaCache(String name, String cfgname, core_id_t core_id, UInt32 block_size, UInt32 size_kb, UInt32 associativity,
                ComponentLatency access_latency, ComponentLatency miss_latency);
    where_t lookup(IntPtr address, SubsecondTime now, bool allocate_on_miss, bool count);
    void allocate(IntPtr address, SubsecondTime now);

    /// Drop the line holding an address (a migrated page's FPA or TAR entry)
    bool invalidate(IntPtr address);

    ComponentLatency getAccessLatency() const { return access_latency; }
    ComponentLatency getMissLatency() const { return miss_latency; }
  };
}

#endif // UTOPIA_CACHE_TEMPLATE_H
