#ifndef BIMODAL_TABLE_H
#define BIMODAL_TABLE_H

#include <boost/scoped_array.hpp>
#include "simulator.h"
#include "branch_predictor.h"
#include "saturating_predictor.h"

class SimpleBimodalTable : BranchPredictor
{

public:

   SimpleBimodalTable(UInt32 entries)
      : m_num_entries(entries)
      , m_table(entries, 0)
   {
      reset();
      m_mask = 0;
      for (unsigned int i = 0 ; i < ilog2(m_num_entries) ; i++)
      {
         m_mask |= (1L<<i);
      }
   }

   bool predict(IntPtr ip, IntPtr target)
   {
      UInt32 index = ip & m_mask;
      return (m_table[index].predict());
   }

   void update(bool predicted, bool actual, IntPtr ip, IntPtr target)
   {
      UInt32 index = ip & m_mask;
      if (actual)
      {
         ++m_table[index];
      }
      else
      {
         --m_table[index];
      }
   }

   void reset()
   {
      for (unsigned int i = 0 ; i < m_num_entries ; i++) {
         m_table[i].reset();
      }
   }

private:

   template<typename Addr>
   Addr ilog2(Addr n)
   {
      Addr i;
      for(i=0;n>0;n>>=1,i++) {}
      return i-1;
   }

private:

   UInt32 m_num_entries;
   IntPtr m_mask;
   std::vector<SaturatingPredictor<2> > m_table;

};

#endif /* BIMODAL_TABLE_H */
