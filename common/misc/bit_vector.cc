#include "bit_vector.h"

/* ================================================================ */
/* Bit vector class method definitions */
/* ================================================================ */


void BitVector::reset()
{
   for (UInt32 i = 0; i < VECTOR_SIZE; i++)
      m_words[i] = 0;

   m_size = 0;
   m_last_pos = -1;
}

/*
 * reset "find". a subsequent call to find()
 * will return the first set bit.
 */
bool BitVector::resetFind()
{
   m_last_pos = -1;
   return true; //maybe do something with this?
}

/*
 * find() will find the next set bit.
 * the BitVector class remembers the last found set bit,
 * so that subsequent calls to find() will start off from
 * the last found set bit (i.e, just keep calling find()
 * to iterate through the set bits in BitVector.
 * Call "resetFind()" to start from the beginning again.
 * However, if you get to the end of the BitVector and
 * keep calling find(), it will wrap around and start over!
 * (b/c if you find no more set bits last_pos is set to -1)
 */

SInt32 BitVector::find()
{

   int windex = (m_last_pos == -1) ? 0 : m_last_pos >> 6; //divide by 64
   int word_count = (m_capacity + 64 -1)  >> 6;

   //walk through bitVector one word at a time
   //return when we find a set bit (whose pos is > last_pos)
   while (windex < word_count)
   {

      UInt64 word64 = m_words[windex];

      if (word64 != 0)
      {

         UInt32 words32[2];
         words32[1] = (word64 >> 32) ;
         words32[0] = word64 & 0xFFFFFFFF;

         for (int i = 0; i < 2; i++)
         {
            if (words32[i] != 0)
            {

               UInt16 words16[2];
               words16[1] = (words32[i] >> 16);
               words16[0] = words32[i] & 0xFFFF;

               for (int j = 0; j < 2; j++)
               {
                  if (words16[j] != 0)
                  {

                     UInt8 words8[2];
                     words8[1] = (words16[j] >> 8);
                     words8[0] = words16[j] & 0xFF;

                     for (int k = 0; k < 2; k++)
                     {
                        if (words8[k] != 0)
                        {

                           for (int l = 0; l < 8; l++)
                           {
                              //loop through byte and look for set bit
                              if (bTestBit(words8[k], l))
                              {
                                 int return_pos = 64*windex + 32*i + 16*j + 8*k + l;
                                 if (return_pos > m_last_pos)
                                 {
                                    m_last_pos = return_pos;
                                    return return_pos;
                                 }
                              }
                           }
                        }
                     }
                  }
               }
            }
         }
      }
      ++windex;
   }

   //if we get here, there is no set bit in bitVector (after the last_pos that is)
   m_last_pos = -1;
   return -1;
}

//helper function to "find", accepts a byte
//and a bit location and returns true if bit is set
bool BitVector::bTestBit(UInt8 byte_word, UInt32 bit)
{
   assert(bit < 8);

// UInt8 shift = bit & 0xF; i don't think this is necessary b/c of assert
   UInt8 one = 1;
   UInt8 mask = one << bit;
   return (byte_word & mask) ? true : false;
}

bool BitVector::at(UInt32 bit)
{
   assert(bit < m_capacity);

   UInt32 index = bit >> 6;
   UInt64 shift = bit & 63;
   UInt64 one   = 1;
   UInt64 mask  = one << shift;
   return (m_words[index] & mask) ? true : false;
}

void BitVector::set(UInt32 bit)
{
   assert(bit < m_capacity);

   UInt32 index  = bit >> 6;
   UInt64 shift  = bit & 63;
   UInt64 one    = 1;
   UInt64 mask   = one << shift;
   if (! at(bit))
   {
      m_words[index] |= mask;
      m_size ++;
   }
}

void BitVector::clear(UInt32 bit)
{
   assert(bit < m_capacity);

   UInt32 index  = bit >> 6;
   UInt64 shift  = bit & 63;
   UInt64 one    = 1;
   UInt64 mask   = ~(one << shift);
   if (at(bit))
   {
      m_words[index] &= mask;
      m_size --;
   }
}

#if BITVECT_DEBUG

void BitVector::set(const BitVector& vec2)
{
   assert(m_capacity == vec2.m_capacity);

   for (UInt32 i = 0; i < VECTOR_SIZE; i++)
      words[i] |= vec2.m_words[i];
}

void BitVector::clear(const BitVector& vec2)
{
   assert(m_capacity == vec2.m_capacity);

   for (UInt32 i = 0; i < VECTOR_SIZE; i++)
      words[i] &= ~vec2.m_words[i];
}

bool BitVector::test(const BitVector& vec2)
{
   assert(vec2.m_capacity == m_capacity);

   for (UInt32 i = 0; i < VECTOR_SIZE; i++)
   {
      if (vec2.m_words[i] & m_words[i])
         return true;
   }

   return false;
}


void BitVector::debug()
{
   assert(m_capacity > 68);

   set(66);
   cout << "set(66) -> " << at(66) << endl;

   clear(66);
   cout << "clear(66) -> " << at(66) << endl;

   set(66);
   set(68);

   BitVector vec2(m_capacity);
   vec2.set(44);

   cout << "test( (1<<66 | 1<<68), (1<<44) ) -> " << test(vec2) << endl;

   vec2.set(66);
   cout << "test( (1<<66 | 1<<68), (1<<66) | (1<<44) ) -> "
        << test(vec2) << endl;

   clear(vec2);
   cout << "test( (1<<68), (1<<66) | (1<<44) ) -> " << test(vec2)
        << endl;

   cout << "at(66) = " << at(66) << "; at(68) = " << at(68) << endl;

   cout << endl;

   //reset();
}

#endif
