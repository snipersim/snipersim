#ifndef GLOBAL_PREDICTOR_H
#define GLOBAL_PREDICTOR_H

#include <vector>
#include <stdint.h>

#include "simulator.h"
#include "branch_predictor.h"
#include "branch_predictor_return_value.h"
#include "saturating_predictor.h"

class GlobalPredictor : BranchPredictor
{

public:

   GlobalPredictor(UInt32 entries, UInt32 tag_bitwidth, UInt32 ways)
      : m_lru_use_count(0)
      , m_num_ways(ways)
      , m_ways(ways, Way(entries/ways, 6))
   {
   }

   bool predict(IntPtr ip, IntPtr target) { LOG_PRINT_ERROR("Need pir"); }
   BranchPredictorReturnValue lookup(IntPtr ip, IntPtr target) { LOG_PRINT_ERROR("Need pir"); }
   void update(bool predicted, bool actual, IntPtr ip, IntPtr target) { LOG_PRINT_ERROR("Need pir"); }

   bool predict(IntPtr ip, IntPtr target, IntPtr pir)
   {
      UInt32 index, tag;

      gen_index_tag(ip, pir, index, tag);

      for (unsigned int w = 0 ; w < m_num_ways ; ++w )
      {
         if (m_ways[w].m_valid[index] && m_ways[w].m_tags[index] == tag)
         {
            return true;
         }
      }
      return false;
   }

   BranchPredictorReturnValue lookup(IntPtr ip, IntPtr target, IntPtr pir)
   {

      UInt32 index, tag;
      BranchPredictorReturnValue ret = { 0, 0, 0, BranchPredictorReturnValue::InvalidBranch };

      gen_index_tag(ip, pir, index, tag);

      for (unsigned int w = 0 ; w < m_num_ways ; ++w )
      {
         if (m_ways[w].m_valid[index] && m_ways[w].m_tags[index] == tag)
         {
            ret.hit = 1;
            ret.prediction = m_ways[w].m_predictors[index].predict();
            break;
         }
      }

      return ret;
   }

   void update(bool predicted, bool actual, IntPtr ip, IntPtr target, IntPtr pir)
   {

      UInt32 index, tag, lru_way;

      gen_index_tag(ip, pir, index, tag);

      // Start with way 0 as the least recently used
      lru_way = 0;

      for (unsigned int w = 0 ; w < m_num_ways ; ++w )
      {
         if (m_ways[w].m_valid[index] && m_ways[w].m_tags[index] == tag)
         {
            m_ways[w].m_predictors[index].update(actual);
            m_ways[w].m_lru[index] = m_lru_use_count++;
            // Once we have a tag match and have updated the LRU information,
            // we can return
            return;
         }

         // Keep track of the LRU in case we do not have a tag match
         if (m_ways[w].m_lru[index] < m_ways[lru_way].m_lru[index])
         {
            lru_way = w;
         }
      }

      // We will get here only if we have not matched the tag
      // If that is the case, select the LRU entry, and update the tag
      // appropriately
      m_ways[lru_way].m_valid[index] = true;
      m_ways[lru_way].m_tags[index] = tag;
      // Here, we miss with the tag, so reset instead of updating
      m_ways[lru_way].m_predictors[index].reset(actual);
      m_ways[lru_way].m_lru[index] = m_lru_use_count++;
   }

   void evict(IntPtr ip, IntPtr pir)
   {
      UInt32 index, tag;

      gen_index_tag(ip, pir, index, tag);

      for (unsigned int w = 0 ; w < m_num_ways ; ++w )
      {
         if (m_ways[w].m_valid[index] && m_ways[w].m_tags[index] == tag)
         {
            m_ways[w].m_valid[index] = false;
            return;
         }
      }
      return;
   }

private:

   class Way
   {
   public:

      Way(UInt32 entries, UInt32 tag_bitwidth)
         : m_valid(entries, false)
         , m_tags(entries, 0)
         , m_predictors(entries, SaturatingPredictor<2>(0))
         , m_lru(entries, 0)
         , m_num_entries(entries)
         , m_tag_bitwidth(tag_bitwidth)
      {
         assert(tag_bitwidth <= 8);
      }

      std::vector<bool> m_valid;
      std::vector<uint8_t> m_tags;
      std::vector<SaturatingPredictor<2> > m_predictors;
      std::vector<UInt64> m_lru;
      UInt32 m_num_entries;
      UInt32 m_tag_bitwidth;

   };

   // Pentium M-specific indexing and tag values
   void gen_index_tag(IntPtr ip, IntPtr pir, UInt32& index, UInt32 &tag)
   {
      index = ((ip >> 4) ^ (pir >> 6)) & 0x1FF;
      tag = ((ip >> 13) ^ pir) & 0x3F;
   }

   UInt64 m_lru_use_count;
   UInt32 m_num_ways;
   std::vector<Way> m_ways;

};

#endif /* GLOBAL_PREDICTOR_H */
