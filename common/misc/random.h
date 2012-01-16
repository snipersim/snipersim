#ifndef __RANDOM_H__
#define __RANDOM_H__

/*
 * Random - A simple random number generator class. Created to avoid
 *   race conditions on rand().
 */

class Random
{
   public:
      typedef UInt32 value_t;

   private:
      value_t _seed;

   public:
      Random() : _seed(1) { }
      ~Random() { }

      inline void seed(value_t s)
      {
         _seed = s;
      }

      inline value_t next(value_t limit = 32768)
      {
         // see rand(3) man page
         const value_t FACTOR = 1103515245;
         const value_t ADDEND = 12345;

         _seed = _seed * FACTOR + ADDEND;
         return (_seed/65536) % limit;
      }
};

#endif // __RANDOM_H__
