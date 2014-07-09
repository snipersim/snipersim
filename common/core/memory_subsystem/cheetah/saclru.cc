/************************************************************************
* Copyright (C) 1989, 1990, 1991, 1992, 1993                            *
*                   Rabin A. Sugumar and Santosh G. Abraham             *
*                                                                       *
* This software is distributed absolutely without warranty. You are     *
* free to use and modify the software as you wish.  You are also free   *
* to distribute the software as long as it is not for commercial gain,  *
* you retain the above copyright notice, and you make clear what your   *
* modifications were.                                                   *
*                                                                       *
* Send comments and bug reports to rabin@eecs.umich.edu                 *
*                                                                       *
* (c) 2013 Wim Heirman <wim@heirman.net>                                *
*            Converted to C++ classes, use 64-bit data types            *
*                                                                       *
************************************************************************/

/* GBT method for simulating set associative caches of fixed line size. */


#define ONE 1U
#define TWO 2
#define B80000000 0x80000000
#define INVALID 0
#define MEM_AVAIL_HITARR 2097152 /* Memory available for hitarr */


#include "saclru.h"
#include "util.h"

#include <cstdlib>
#include <cassert>


CheetahSACLRU::CheetahSACLRU(int _N, int _B, int _A, int _L)
   : N(_N), B(_B), A(_A), L(_L)
   , SAVE_INTERVAL(0), P_INTERVAL(0) /* unsupported for now */
   , t_entries(0), depths(NULL), hitarr0(0), tag(0)
{
   init_saclru();
}

CheetahSACLRU::~CheetahSACLRU()
{/*
   free(arr);
   free(hitarr);
   free(base_pwr_array);
   free(rm_arr);
   free(depths);
   free(sac_hits[0]);
   free(sac_hits);*/
}


void
CheetahSACLRU::flush(void)
{
  for (unsigned i=0; i<=MAX_DEPTH;i++)
    sac_hits[0][i] += hitarr0;
  hitarr0 = 0;
}


/****************************************************************
Output routine.

Input: None
Output: None
Side effects: None
****************************************************************/
void
CheetahSACLRU::outpr_saclru(FILE *fd)
{
  unsigned i, j;
  uint64_t sum;

  flush();

  fprintf(fd, "Addresses processed: %ld\n", t_entries);
  fprintf(fd, "Line size: %d bytes\n", (ONE << L));
#ifdef PERF
  fprintf(fd, "compares  %d\n", compares);
#endif
  fprintf(fd, "\n");
  fprintf(fd, "Miss Ratios\n");
  fprintf(fd, "___________\n\n");
  fprintf(fd, "\t\tAssociativity\n");
  fprintf(fd, "\t\t");
  for (i=0;i<TWO_PWR_N;i++)
    fprintf(fd, "%d\t\t", (i+1));
  fprintf(fd, "\n");
  fprintf(fd, "No. of sets\n");
  for (i=0; i <= MAX_DEPTH; i++)
    {
      sum = 0;
      fprintf(fd, "%d\t\t", (ONE << (i+A)));
      for (j=0; j < TWO_PWR_N; j++)
        {
          sum += sac_hits[j][i];
          fprintf(fd, "%f\t", (1.0 - ((double)sum/(double)t_entries)));
        }
      fprintf(fd, "\n");
    }
  fprintf(fd, "\n");
}


uint64_t
CheetahSACLRU::hits(unsigned sets_log2, unsigned assoc)
{
   assert(sets_log2 >= A);
   assert(sets_log2 <= B);

   if (hitarr0)
      flush();

   uint64_t hits = 0;
   for(unsigned int j = 0; j < assoc; ++j)
      hits += sac_hits[j][sets_log2-A];

   return hits;
}


/**********************************************************************
Initialization routine. Allocates space for the various arrays and
initializes them.

Input: None
Output: None
Side effects: Allocates space for the arrays and initializes the array
         locations.
**********************************************************************/
void
CheetahSACLRU::init_saclru(void)
{
  uint64_t i, j, k, l;
  uint64_t init_value;
  uint64_t *arr_ptr, *slot_ptr;

  next_save_time = SAVE_INTERVAL;
  TWO_PWR_N = (ONE << N);
  MAX_DEPTH = B-A;
  TWO_POWER_MAX_DEPTH = (ONE << MAX_DEPTH);
  SET_MASK = ((ONE << A) - 1);
  DIFF_SET_MASK = ((ONE << MAX_DEPTH) - 1);
  BASE = (TWO_PWR_N+1);
  SIZE_OF_TREE = (TWO_POWER_MAX_DEPTH * TWO * BASE);

  assert(((uint64_t)(ONE << A)*TWO_POWER_MAX_DEPTH*TWO*BASE) < (uint64_t(1)<<32));
  arr = (uint64_t *)calloc((uint64_t(ONE << A)*TWO_POWER_MAX_DEPTH*TWO*BASE), sizeof(uint64_t));
  if (!arr)
    fatal("out of virtual memory");

  BASE_PWR_MAX_DEPTH_PLUS_ONE = power(BASE, MAX_DEPTH+1);
  if ((BASE_PWR_MAX_DEPTH_PLUS_ONE * sizeof(uint64_t)) < MEM_AVAIL_HITARR)
    {
      hitarr = (int64_t *)calloc(BASE_PWR_MAX_DEPTH_PLUS_ONE, sizeof(int64_t));
      if (!hitarr)
        fatal("out of virtual memory");
    }

  base_pwr_array = (uint64_t *)calloc((MAX_DEPTH+1), sizeof(uint64_t));
  if (!base_pwr_array)
    fatal("out of virtual memory");

  rm_arr = (uint64_t *)calloc((TWO_POWER_MAX_DEPTH), sizeof(uint64_t));
  if (!rm_arr)
    fatal("out of virtual memory");

  sac_hits = idim2((TWO_PWR_N), (MAX_DEPTH + 2));

  for (i=0; i < ONE << A; i++)
    {
      arr_ptr = arr + i * SIZE_OF_TREE;
      init_value = B80000000;
      slot_ptr = arr_ptr;
      *slot_ptr = 1;
      for (l=1; l<=TWO_PWR_N; l++)
        *(slot_ptr + l) = init_value;
      ++init_value;
      for (j=1; j <= MAX_DEPTH; j++)
        {
          for (k=((ONE << j) - 1);
               k < (((ONE << j) - 1) + (ONE << (j-1)));
               k++)
            {
              *(arr_ptr + (k*(TWO_PWR_N+1))) = TWO_PWR_N+1;
            }
          for (k= (((ONE << j) - 1) + (ONE << (j-1)));
               k < ((ONE << (j+1)) - 1);
               k++)
            {
              slot_ptr = arr_ptr + (k * (TWO_PWR_N+1));
              *slot_ptr = 1;
              for (l=1; l<=TWO_PWR_N; l++)
                *(slot_ptr + l) = init_value;
              ++init_value;
            }
        }
    }
  rm_arr[0] = MAX_DEPTH;
  for (i=1; i < (ONE << MAX_DEPTH); i++)
    {
      for (j=0; j<=MAX_DEPTH; j++)
        {
          if (i & (ONE << j))
            {
              rm_arr[i] = j;
              break;
            }
        }
    }
  j = 1;
  for (i=0; i<=MAX_DEPTH; i++)
    {
      base_pwr_array[i] = j;
      j *= BASE;
    }

  depths = (uint64_t *)calloc((MAX_DEPTH+1), sizeof(uint64_t));
  if (!depths)
    fatal("out of virtual memory");

  for (i=0;i<=MAX_DEPTH;i++)
    depths[i] = 0;
}


/*****************************************************************
Main simulation routine used when size of hitarr exceeds memory limit.
Does a GBT lookup and update as similar to 'sacnmul' but updates
the 'sac_hits' array directly instead of 'hitarr'.

Input: None
Output: None
Side effects: Updates GBT. Increments 'sac_hits' array locations.
*****************************************************************/
void
CheetahSACLRU::sacnmul_woarr(intptr_t addr)
{
  uint64_t t, t1;
  int i, fst;
  uint64_t orig_tag,  atag, depth, entry, set_no;
  uint64_t sum, hit;
  uint64_t *arr_ptr, *slot_ptr;

#if 0
  if (t_entries > next_save_time)
    {
#if 0 /* tma: is this needed? */
      for (i=0; i<=MAX_DEPTH;i++)
        sac_hits[0][i] += hitarr0;
      hitarr0 = 0;
#endif
      outpr_saclru(stderr);
      next_save_time += SAVE_INTERVAL;
    }
#endif

  ++t_entries;
#if 0
  if ((t_entries % P_INTERVAL) == 0)
    fprintf(stderr, "libcheetah: addresses processed %d\n", t_entries);
#endif

  addr >>= L;
  set_no = addr & SET_MASK;
  arr_ptr = arr + set_no * SIZE_OF_TREE;
  orig_tag = addr >> A;
#ifdef PERF
  ++compares;
#endif
  if (*(arr_ptr + 1) == orig_tag)
    ++hitarr0;
  else
    {
      atag = orig_tag;
      depth = 0;
      hit = 0;
      fst = 1;
      slot_ptr = arr_ptr;
      while (depth <= MAX_DEPTH)
        {
          for (i=fst; i<=(int)TWO_PWR_N; i++)
            {
#ifdef PERF
              ++compares;
#endif
              if ((t1 = *(slot_ptr + i)) == orig_tag)
                {
                  *(slot_ptr+i) = tag;
                  hit = 1;
                  break;
                }
              t = rm_arr[(orig_tag ^ t1) & DIFF_SET_MASK];
              ++depths[t];
              *(slot_ptr + i) = tag;
              tag = t1;
            }
          ++*slot_ptr;
          entry =
            (((ONE << depth) + (atag & ((ONE << depth) - 1))) - 1) * BASE;
          slot_ptr = arr_ptr + entry;
          --*slot_ptr;
          slot_ptr[*slot_ptr] = atag;
          if (hit==1)
            {
              sum = 0;
              i = MAX_DEPTH;
              while (i >= 0)
                {
                  sum += depths[i];
                  if (sum < TWO_PWR_N)
                    {
                      ++sac_hits[sum][i];
                      --i;
                    }
                  else
                    break;
                }
              break;
            }
          ++depth;
          atag = tag;
          entry =
            (((ONE << depth) + (orig_tag & ((ONE << depth)-1)))-1) * BASE;
          while ((depth <= MAX_DEPTH) && (*(arr_ptr + entry) > TWO_PWR_N))
            {
              ++depth;
              entry =
                (((ONE << depth) + (orig_tag & ((ONE << depth)-1)))-1) * BASE;
            }
          if (depth <= MAX_DEPTH)
            {
              slot_ptr = arr_ptr + entry;
              fst = *slot_ptr;
            }
        }

      for (i=0;i<=(int)MAX_DEPTH;i++)
        depths[i] = 0;
    } /* else */
}
