#ifndef __CACHE_PERF_MODEL_H__
#define __CACHE_PERF_MODEL_H__

#include "fixed_types.h"
#include "subsecond_time.h"

class CachePerfModel
{
   public:
      enum CacheAccess_t
      {
         ACCESS_CACHE_DATA_AND_TAGS = 0,
         ACCESS_CACHE_DATA,
         ACCESS_CACHE_TAGS,
         NUM_CACHE_ACCESS_TYPES
      };

      enum PerfModel_t
      {
         CACHE_PERF_MODEL_PARALLEL = 0,
         CACHE_PERF_MODEL_SEQUENTIAL,
         NUM_CACHE_PERF_MODELS
      };

   protected:
      ComponentLatency m_cache_data_access_time;
      ComponentLatency m_cache_tags_access_time;

   public:
      CachePerfModel(const ComponentLatency& cache_data_access_time, const ComponentLatency& cache_tags_access_time);
      virtual ~CachePerfModel();

      static CachePerfModel* create(String cache_perf_model_type,
            const ComponentLatency& cache_data_access_time,
            const ComponentLatency& cache_tags_access_time);
      static PerfModel_t parseModelType(String model_type);

      virtual void enable() = 0;
      virtual void disable() = 0;
      virtual bool isEnabled() = 0;

      virtual SubsecondTime getLatency(CacheAccess_t access) = 0;
};

#endif /* __CACHE_PERF_MODEL_H__ */
