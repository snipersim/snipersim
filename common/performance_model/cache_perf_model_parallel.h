#ifndef __CACHE_PERF_MODEL_PARALLEL_H__ 
#define __CACHE_PERF_MODEL_PARALLEL_H__

#include "cache_perf_model.h"

class CachePerfModelParallel : public CachePerfModel
{
   private:
      bool m_enabled;

   public:
      CachePerfModelParallel(const ComponentLatency& cache_data_access_time,
            const ComponentLatency& cache_tags_access_time) :
         CachePerfModel(cache_data_access_time, cache_tags_access_time),
         m_enabled(false)
      {}
      ~CachePerfModelParallel() {}

      void enable() { m_enabled = true; }
      void disable() { m_enabled = false; }
      bool isEnabled() { return m_enabled; }

      SubsecondTime getLatency(CacheAccess_t access)
      {
         if (!m_enabled)
            return SubsecondTime::Zero();

         switch(access)
         {
            case ACCESS_CACHE_TAGS:
               return m_cache_tags_access_time.getLatency();

            case ACCESS_CACHE_DATA:
            case ACCESS_CACHE_DATA_AND_TAGS:
               return m_cache_data_access_time.getLatency();

            default:
               return SubsecondTime::Zero();
         }
      }
 
};

#endif /* __CACHE_PERF_MODEL_PARALLEL_H__ */
