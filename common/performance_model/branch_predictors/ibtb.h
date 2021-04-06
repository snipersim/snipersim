#ifndef SIMPLE_INDIRECT_BRANCH_TARGET_BUFFER_H
#define SIMPLE_INDIRECT_BRANCH_TARGET_BUFFER_H

//#include <boost/scoped_array.hpp>

#include "simulator.h"
#include "branch_predictor.h"
#include <vector>

class IndirectBranchTargetBuffer : BranchPredictor
{

  public:
  IndirectBranchTargetBuffer(UInt32 entries) : m_num_entries(entries)
       , m_table(entries, std::make_tuple(UInt32(0), IntPtr(0)))
  {
    lru = 0;
    history = 0x00000FFF;
  }

  // Report if we have a iBTB hit
  // Index = [13:6]
  // Tag = [14,5:0]
  bool predict(bool indirect, IntPtr ip, IntPtr target)
  {
    UInt32 index;
    index = ip ^ history;
    index = ip;
    target = target;
    for (UInt32 i = 0; i < m_num_entries; i++) {
      if (std::get<0>(m_table[i]) == index) {
        if (std::get<1>(m_table[i]) == target) {
          return true;
        } else {
          return false;
        }
      }
    }

    return false;
  }

  void update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target)
  {

    UInt32 index;

    if (actual == false) {
      history = history >> 1;
      return;
    } else {

      index = ip ^ history;
      index = ip;
      bool done = false;
      for (UInt32 i = 0; i < m_num_entries; i++) {
        if (std::get<0>(m_table[i]) == index || std::get<0>(m_table[i]) == 0) {
          std::get<0>(m_table[i]) = index;
          std::get<1>(m_table[i]) = target;
          done = true;
        }
      }
      if (done == false) {
        std::get<0>(m_table[lru]) = index;
        std::get<1>(m_table[lru]) = target;
        lru = (lru + 1) % m_num_entries;
      }

      history = history >> 1;
      history |= 0x00000800;
    }
  }

  private:
  UInt32 m_num_entries;
  UInt32 history;
  int lru;
  std::vector<std::tuple<UInt32, IntPtr> > m_table;

  void gen_index_tag(IntPtr ip, UInt32& index, UInt32& tag)
  {
    index = (ip >> 6) & 0xFF;
    tag = ((ip & 0x4000) >> 8) | (ip & 0x3F);
  }
};

#endif /* SIMPLE_INDIRECT_BRANCH_TARGET_BUFFER_H */
