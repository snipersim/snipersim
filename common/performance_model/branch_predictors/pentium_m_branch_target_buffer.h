#ifndef PENTIUM_M_BRANCH_TARGET_BUFFER_H
#define PENTIUM_M_BRANCH_TARGET_BUFFER_H

#include <vector>

#include "branch_predictor.h"

#define NUM_WAYS 4
#define NUM_ENTRIES 512

class PentiumMBranchTargetBuffer : BranchPredictor
{
   #define IP_TO_INDEX(_ip) ((_ip >> 4) & 0x1ff)

   #define TAG_OFFSET_MASK 0x3fe00f
   #define IP_TO_TAGOFF(_ip) (_ip & TAG_OFFSET_MASK)
   // offset = ip[3:0] (4 bits)
   // index = ip[12:4] (9 bits), 512 entries
   // tag = ip[21:13] (9 bits)
   class Way
   {
   public:
      Way()
         : m_tag_offset(NUM_ENTRIES, 0)
         , m_plru(NUM_ENTRIES, 0)
      {}

      std::vector<UInt32> m_tag_offset; // tag and offset data
      std::vector<UInt64> m_plru; // Should be pseudo-LRU, using LRU instead
   };

public:
   PentiumMBranchTargetBuffer()
      : m_ways(NUM_WAYS)
      , m_lru_use_count(0)
   {}

   bool predict(IntPtr ip, IntPtr target)
   {
      return false;
   }

   BranchPredictorReturnValue lookup(IntPtr ip, IntPtr target)
   {
      bool hit = false;
      UInt32 tag_offset = IP_TO_TAGOFF(ip);
      UInt32 index = IP_TO_INDEX(ip);
      for (UInt32 i = 0 ; i < NUM_WAYS ; i++)
      {
         if (m_ways[i].m_tag_offset[index] == tag_offset)
         {
            hit = true;
            break;
         }
      }

      BranchPredictorReturnValue ret = { false, hit, 0, BranchPredictorReturnValue::InvalidBranch };

      return ret;
   }

   void update(bool predicted, bool actual, IntPtr ip, IntPtr target)
   {
      // Start with way 0 as the least recently used
      UInt32 lru_way = 0;

      UInt32 tag_offset = IP_TO_TAGOFF(ip);
      UInt32 index = IP_TO_INDEX(ip);
      for (unsigned int w = 0 ; w < NUM_WAYS ; ++w )
      {
         if (m_ways[w].m_tag_offset[index] == tag_offset)
         {
            m_ways[w].m_plru[index] = m_lru_use_count++;
            // Once we have a tag match and have updated the LRU information,
            // we can return
            return;
         }

         // Keep track of the LRU in case we do not have a tag match
         if (m_ways[w].m_plru[index] < m_ways[lru_way].m_plru[index])
         {
            lru_way = w;
         }
      }

      // We will get here only if we have not matched the tag
      // If that is the case, select the LRU entry, and update the tag
      // appropriately
      m_ways[lru_way].m_tag_offset[index] = tag_offset;
      m_ways[lru_way].m_plru[index] = m_lru_use_count++;
   }

private:
   std::vector<Way> m_ways;
   UInt64 m_lru_use_count;

};

#endif /* PENTIUM_M_BRANCH_TARGET_BUFFER_H */

