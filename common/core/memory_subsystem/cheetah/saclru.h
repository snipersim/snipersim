#ifndef __SACLRU_H
#define __SACLRU_H

#include <cstdint>
#include <cstdio>

class CheetahSACLRU
{
   private:
      const unsigned int N,     /* Degree of associativity = 2^N */
                         B,     /* Max set field width */
                         A,     /* Min set field width */
                         L,     /* Line field width */
                         //T,     /* Max no of addresses to be processed */
                         SAVE_INTERVAL, /* Intervals at which output should be saved */
                         P_INTERVAL;  /* Intervals at which progress output is done */

       unsigned TWO_PWR_N; /* 2^N. i.e., Degree of associativity */
       unsigned MAX_DEPTH;   /* B-A, Number of range of sets simulated */
       int TWO_POWER_MAX_DEPTH,  /* 2^MAX_DEPTH */
           SET_MASK, /* Masks */
           DIFF_SET_MASK,
           BASE,  /* 2^N+1, |{0,1, ..., 2^N}| */
           SIZE_OF_TREE,   /* 2^MAX_DEPTH * 2 * BASE , No of nodes in tree
            There is a factor of two redundancy in the array.
            BASE gives an extra location used to indicate
            the number of entries in a row. */
           BASE_PWR_MAX_DEPTH_PLUS_ONE; /* Number of locations required for hitarr */

       uint64_t *arr;   /* GBT array */
       uint64_t **sac_hits; /* Hit counts in caches */
       int64_t *hitarr; /* Encoded hit counts */

       uint64_t *base_pwr_array;/* Stores BASE^0, BASE^1,..., BASE^MAX_DEPTH */
       uint64_t *rm_arr;   /* Stores right-matches */

       uint64_t t_entries; /* Count of addresses processed */

       unsigned long next_save_time;
       uint64_t *depths;  /* Right-match count array */
       uint64_t hitarr0;
       uint64_t tag;

   public:
      CheetahSACLRU(int _N,     /* Degree of associativity = 2^N */
                    int _B,     /* Max set field width */
                    int _A,     /* Min set field width */
                    int _L      /* Line field width */
      );
      ~CheetahSACLRU();

      void outpr_saclru(FILE *fd);
      void init_saclru(void);
      void sacnmul_woarr(intptr_t addr);
      void flush(void);

      uint64_t hits(unsigned sets_log2, unsigned assoc);
      uint64_t numentries(void) { return t_entries; }
};

#endif // __SACLRU_H
