#ifndef SIMPLE_INDIRECT_BRANCH_TARGET_BUFFER_H
#define SIMPLE_INDIRECT_BRANCH_TARGET_BUFFER_H

#include <boost/scoped_array.hpp>

#include "simulator.h"
#include "branch_predictor.h"

class IndirectBranchTargetBuffer : BranchPredictor
{

public:

   IndirectBranchTargetBuffer(UInt32 entries, UInt32 tag_bitwidth)
      : m_num_entries(entries)
      , m_tag_bitwidth(tag_bitwidth)
      , m_table(new uint8_t[entries])
   {
      assert(tag_bitwidth <= 8 && tag_bitwidth >= 0);
      for (unsigned int i = 0 ; i < entries ; i++) {
         m_table[i] = 0;
      }
   }

   // Report if we have a iBTB hit
   // Index = [13:6]
   // Tag = [14,5:0]
   bool predict(IntPtr ip, IntPtr target)
   {
      UInt32 index, tag;

      gen_index_tag(ip, index, tag);

      if (m_table[index] == tag)
      {
         return true;
      }
      else
      {
         return false;
      }
   }

   void update(bool predicted, bool actual, IntPtr ip, IntPtr target)
   {

      UInt32 index, tag;

      gen_index_tag(ip, index, tag);

      m_table[index] = tag;

   }

private:

   UInt32 m_num_entries;
   UInt32 m_tag_bitwidth;
   boost::scoped_array<uint8_t> m_table;

   void gen_index_tag(IntPtr ip, UInt32& index, UInt32 &tag)
   {
      index = (ip >> 6) & 0xFF;
      tag = ((ip & 0x4000) >> 8) | (ip & 0x3F);
   }

};

#endif /* SIMPLE_INDIRECT_BRANCH_TARGET_BUFFER_H */
