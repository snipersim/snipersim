// Jonathan Eastep (eastep@mit.edu)
// 10.10.08
// This file provides a bit vector class


#ifndef BIT_VECTOR_H
#define BIT_VECTOR_H


#include "fixed_types.h"
#include <assert.h>
#include <vector>


#define BITVECT_DEBUG 0

class BitVector
{
   private:
      UInt32 m_capacity;
      UInt32 m_size;
      SInt32 m_last_pos; 
      const UInt32 VECTOR_SIZE;
      std::vector<UInt64> m_words;
      //used in find function, for iterating through the set bits
      //marks the position of the last set bit found.
      //value of -1 means no last_pos bit

   public:

#if BITVECT_DEBUG
      void debug();
#endif

      BitVector(UInt32 bits): 
         m_capacity(bits),
         m_size(0),
         m_last_pos(-1),
         VECTOR_SIZE((bits + 64 - 1) >> 6)
      {
         m_words.resize(VECTOR_SIZE);
      }
      ~BitVector() {}

      //starting from position 'last_pos', find the next "1" bit.
      //private variable 'last_pos' remembers the last position found,
      //allowing subsequent calls to "find" to effectively iterate
      //through the entire BitVector
      //returns -1 if no bits set (after last_pos)

      SInt32 find();
      bool resetFind();

      //given an 8bit word, test to see if 'bit' is set
      //this is a helper function to the "find" function
      bool bTestBit(UInt8 word, UInt32 bit);

      UInt32 capacity() { return m_capacity; }
      UInt32 size() { return m_size; }

      void reset();
      bool at(UInt32 bit);
      void set(UInt32 bit);
      void clear(UInt32 bit);

#ifdef BITVECT_DEBUG
      void set(const BitVector& vec2);
      void clear(const BitVector& vec2);
      bool test(const BitVector& vec2);
#endif

};

#endif
