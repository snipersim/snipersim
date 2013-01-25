#include "cache_set.h"
#include "log.h"

// Tree LRU for 4 and 8 way caches

CacheSetPLRU::CacheSetPLRU(
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize) :
   CacheSet(cache_type, associativity, blocksize)
{
   LOG_ASSERT_ERROR(associativity == 4 || associativity == 8,
      "PLRU not implemted for associativity %d (only 4, 8)", associativity);
   for(int i = 0; i < 8; ++i)
      b[i] = 0;
}

CacheSetPLRU::~CacheSetPLRU()
{
}

UInt32
CacheSetPLRU::getReplacementIndex()
{
   // Invalidations may mess up the LRU bits

   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
         return i;
   }

   if (m_associativity == 4)
   {
      if (b[0] == 0)
      {
         if (b[1] == 0) return 0;
         else           return 1;   // b1==1
      }
      else
      {
         if (b[2] == 0) return 2;
         else           return 3;   // b2==1
      }
   }
   else if (m_associativity == 8)
   {
      if (b[0] == 0)
      {
         if (b[1] == 0)
         {
            if (b[2] == 0) return 0;
            else           return 1;  // b2==1
         }
         else
         {                            // b1==1
            if (b[3] == 0) return 2;
            else           return 3;  // b3==1
         }
      }
      else
      {                               // b0==1
         if (b[4] == 0)
         {
            if (b[5] == 0) return 4;
            else           return 5;  // b5==1
         }
         else
         {                            // b4==1
            if (b[6] == 0) return 6;
            else           return 7;  // b6==1
         }
      }
   }
   else
   {
      LOG_PRINT_ERROR("PLRU doesn't support associativity %d", m_associativity);
   }

   LOG_PRINT_ERROR("Error Finding LRU bits");
}

void
CacheSetPLRU::updateReplacementIndex(UInt32 accessed_index)
{
   if (m_associativity == 4)
   {
      if      (accessed_index==0) { b[0]=1;b[1]=1;     }
      else if (accessed_index==1) { b[0]=1;b[1]=0;     }
      else if (accessed_index==2) { b[0]=0;       b[2]=1;}
      else if (accessed_index==3) { b[0]=0;       b[2]=0;}
   }
   else if (m_associativity == 8)
   {
      if      (accessed_index==0) { b[0]=1;b[1]=1;b[2]=1;                            }
      else if (accessed_index==1) { b[0]=1;b[1]=1;b[2]=0;                            }
      else if (accessed_index==2) { b[0]=1;b[1]=0;       b[3]=1;                     }
      else if (accessed_index==3) { b[0]=1;b[1]=0;       b[3]=0;                     }
      else if (accessed_index==4) { b[0]=0;                     b[4]=1;b[5]=1;       }
      else if (accessed_index==5) { b[0]=0;                     b[4]=1;b[5]=0;       }
      else if (accessed_index==6) { b[0]=0;                     b[4]=0;       b[6]=1;}
      else if (accessed_index==7) { b[0]=0;                     b[4]=0;       b[6]=0;}
   }
   else
   {
      LOG_PRINT_ERROR("PLRU doesn't support associativity %d", m_associativity);
   }
}
