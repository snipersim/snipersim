#ifndef __CACHE_PERF_MODEL_SEQUENTIAL_H__ 
#define __CACHE_PERF_MODEL_SEQUENTIAL_H__

#include "cache_perf_model.h"

class CachePerfModelSequential : public CachePerfModel
{
   private:
      bool m_enabled;

   public:
      CachePerfModelSequential(const ComponentLatency& cache_data_access_time,
            const ComponentLatency& cache_tags_access_time) :
         CachePerfModel(cache_data_access_time, cache_tags_access_time),
         m_enabled(false)
      {}
      ~CachePerfModelSequential() {}

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
               return m_cache_data_access_time.getLatency();

            case ACCESS_CACHE_DATA_AND_TAGS:
               return m_cache_data_access_time.getLatency() + m_cache_tags_access_time.getLatency();

            default:
               return SubsecondTime::Zero();
         }
      }
};

#endif /* __CACHE_PERF_MODEL_SEQUENTIAL_H__ */
