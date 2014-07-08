/************************************************************************
* Copyright (C) 1989, 1990, 1991, 1992, 1993                    	*
*                   Rabin A. Sugumar and Santosh G. Abraham		*
*									*
* This software is distributed absolutely without warranty. You are	*
* free to use and modify the software as you wish.  You are also free	*
* to distribute the software as long as it is not for commercial gain,	*
* you retain the above copyright notice, and you make clear what your	*
* modifications were.							*
*									*
* Send comments and bug reports to rabin@eecs.umich.edu			*
*									*
************************************************************************/

/* GBT method for simulating set associative caches of fixed line size.	*/


#include <stdio.h>
#include <stdlib.h>

#include "../host.h"
#include "../misc.h"
#include "../machine.h"
#include "util.h"
#include "libcheetah.h"

#define ONE 1U
#define TWO 2
#define B80000000 0x80000000
#define INVALID 0
#define MEM_AVAIL_HITARR 2097152	/* Memory available for hitarr */

extern int N,		/* Degree of associativity = 2^N */
           B,		/* Max set field width */
           A,		/* Min set field width */
           L,		/* Line field width */
           T,		/* Max no of addresses to be processed */
           SAVE_INTERVAL, /* Intervals at which output should be saved */
           P_INTERVAL;  /* Intervals at which progress output is done */

static unsigned TWO_PWR_N;	/* 2^N. i.e., Degree of associativity */
static unsigned MAX_DEPTH;   /* B-A, Number of range of sets simulated */
static int TWO_POWER_MAX_DEPTH,	/* 2^MAX_DEPTH */
           SET_MASK,	/* Masks */
           DIFF_SET_MASK,
           BASE,	/* 2^N+1, |{0,1, ..., 2^N}| */
           SIZE_OF_TREE,	/* 2^MAX_DEPTH * 2 * BASE , No of nodes in tree
			   There is a factor of two redundancy in the array.
			   BASE gives an extra location used to indicate
			   the number of entries in a row. */
           BASE_PWR_MAX_DEPTH_PLUS_ONE; /* Number of locations required for hitarr */

static unsigned *arr;	/* GBT array */
unsigned **sac_hits; /* Hit counts in caches */
static int *hitarr; /* Encoded hit counts */

static unsigned *base_pwr_array;/* Stores BASE^0, BASE^1,..., BASE^MAX_DEPTH */
static unsigned *rm_arr;	/* Stores right-matches */

static unsigned t_entries; /* Count of addresses processed */
#ifdef PERF
int compares=0;		/* Number of comparisons */
#endif

static unsigned long next_save_time;
static unsigned *depths = NULL;	/* Right-match count array */
int hitarr0 = 0;
unsigned tag = 0;


/****************************************************************
Output routine.

Input: None
Output: None
Side effects: None
****************************************************************/
void
outpr_saclru(FILE *fd)
{
  unsigned i, j;
  int sum;

  for (i=0; i<=MAX_DEPTH;i++)
    sac_hits[0][i] += hitarr0;
  hitarr0 = 0;
  
  fprintf(fd, "Addresses processed: %d\n", t_entries);
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


/**********************************************************************
Initialization routine. Allocates space for the various arrays and
initializes them.

Input: None
Output: None
Side effects: Allocates space for the arrays and initializes the array
	      locations.
**********************************************************************/
void     
init_saclru(void)
{
  unsigned i, j, k, l;
  unsigned init_value;
  unsigned *arr_ptr, *slot_ptr;

  next_save_time = SAVE_INTERVAL;
  TWO_PWR_N = (ONE << N);
  MAX_DEPTH = B-A;
  TWO_POWER_MAX_DEPTH = (ONE << MAX_DEPTH);
  SET_MASK = ((ONE << A) - 1);
  DIFF_SET_MASK = ((ONE << MAX_DEPTH) - 1);
  BASE = (TWO_PWR_N+1);
  SIZE_OF_TREE = (TWO_POWER_MAX_DEPTH * TWO * BASE);

  arr = calloc(((ONE << A)*TWO_POWER_MAX_DEPTH*TWO*BASE), sizeof(int));
  if (!arr)
    fatal("out of virtual memory");

  BASE_PWR_MAX_DEPTH_PLUS_ONE = power(BASE, MAX_DEPTH+1);
  if ((BASE_PWR_MAX_DEPTH_PLUS_ONE * sizeof(unsigned)) < MEM_AVAIL_HITARR)
    {
      hitarr = calloc(BASE_PWR_MAX_DEPTH_PLUS_ONE, sizeof(int));
      if (!hitarr)
	fatal("out of virtual memory");
    }

  base_pwr_array = calloc((MAX_DEPTH+1), sizeof(unsigned));
  if (!base_pwr_array)
    fatal("out of virtual memory");

  rm_arr = calloc((TWO_POWER_MAX_DEPTH), sizeof(unsigned));
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

  depths = calloc((MAX_DEPTH+1), sizeof(unsigned));
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
sacnmul_woarr(md_addr_t addr)
{
  unsigned t, t1;
  int i, fst;
  unsigned orig_tag,  atag, depth, entry, set_no; 
  unsigned sum, hit;
  unsigned *arr_ptr, *slot_ptr;
  
  
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

  ++t_entries;
  if ((t_entries % P_INTERVAL) == 0)
    fprintf(stderr, "libcheetah: addresses processed %d\n", t_entries);
      
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


#if 0

/*****************************************************************
Main simulation routine. Reads in addresses from trace and does
a GBT lookup and update for each.

Input: None
Output: None
Side effects: Updates GBT. Increments hitarr locations.
*****************************************************************/

/****************************************************************
Processes the hitarr (when sacnmul is used) to determine sac_hits
in various caches.

Input: None
Output: None
Side effects: Sets the locations of the 'sac_hits' array based on the
	      counts in hitarr.
****************************************************************/

postproc()
     
{  int index, depth, j, k;
   int *digit, *depth_sum;

   digit = (int *)calloc((MAX_DEPTH + 1), sizeof(int));
   depth_sum = (int *)calloc((MAX_DEPTH + 1), sizeof(int));

   for (j=0;j<=MAX_DEPTH;j++)
     sac_hits[0][j] = hitarr[0];
   index = 0;
   depth = 0;
   for (j=0; j <= MAX_DEPTH; j++) {
      digit[j] = 0;
      depth_sum[j] = 0;
   }
   while (depth <= MAX_DEPTH) {
      if (digit[depth] == (BASE - 1)) {
	digit[depth] = 0;
	for (j=depth; j >= 0; j--)
	   depth_sum[j] -= BASE - 1;
	++depth;
      }
      else {
	++digit[depth];
	++index;
	for (j=depth; j >= 0; j--)
	   ++depth_sum[j];
	depth = 0;
	for (k=MAX_DEPTH; k >= 0; k--)
	   if (depth_sum[k] < TWO_PWR_N)
	     sac_hits[depth_sum[k]][k] += hitarr[index];
	   else
		break;
      }
   }
#ifdef PERF
   printf("index  %d\n", index);
#endif
}

sacnmul()

{  unsigned t, t1;
   int i,
       fst;		/* Index to first entry in current row 
  row is like @00xx, where 0 empty; x occupied; @ index to first, here 3 */
   unsigned orig_tag, /* Tag of address in smallest caches */
	    atag,	/* Tag removed from previous level */
	    tag,	/* Tag removed from previous entry in row */
	    depth,	/* Current level */
	    entry,	/* (arr_ptr + entry) points to current row */
	    set_no,	/* GBT number */
	    addr;	/* Current trace address */
   unsigned depths;	/* hitarr index */
   short hit;		/* Flag */
   unsigned *arr_ptr,	/* Pointer to top of current GBF */
	   *slot_ptr;	/* Pointer to current row */
   int hitarr0;		/* hitarr[0]; to avoid an array access */
   
   unsigned *buffer;	/* Input buffer */
   unsigned l, nr;		/* Ints related to buffer handling */

   hitarr0 = 0;
   
   while (nr = trace(&buffer)) {
     for (l = 0; l < nr; l++) {
     ++t_entries;
     if ((t_entries % P_INTERVAL) == 0)
       fprintf(stderr, "libcheetah: addresses processed %d\n", t_entries);

       addr = *(buffer + l);
       /*  while (fscanf(stdin, "%d", &addr) != EOF) { */
       /*	if ((t_entries % 10000) == 0)
		printf("t_entries  %d\n", t_entries); */
       addr >>= L;
       set_no = addr & SET_MASK;
       arr_ptr = arr + set_no * SIZE_OF_TREE;
       orig_tag = addr >> A;
#ifdef PERF
	++compares;
#endif
       if (*(arr_ptr + 1) == orig_tag)
	/* Hit at root */
         ++hitarr0;
       else {
         atag = orig_tag;
         depth = 0;
	 depths = 0;
	 hit = 0;
	 fst = 1;
	 slot_ptr = arr_ptr;
	 while (depth <= MAX_DEPTH) {
	    for (i=fst; i<=TWO_PWR_N; i++) {
#ifdef PERF
	    ++compares;
#endif
	      if ((t1 = *(slot_ptr + i)) == orig_tag) {
	/* Hit at a node below the root */
		 *(slot_ptr+i) = tag;
		 hit = 1;
		 break;
	      }
	      t = rm_arr[(orig_tag ^ t1) & DIFF_SET_MASK];
	      depths += base_pwr_array[t];
	      *(slot_ptr + i) = tag;
	      tag = t1;
	    }
	    ++*slot_ptr;
	    entry = (((ONE << depth) + (atag & ((ONE << depth) - 1))) - 1) * BASE;
	    slot_ptr = arr_ptr + entry;
	    --*slot_ptr;
	    slot_ptr[*slot_ptr] = atag;
	    if (hit==1) {
	       ++hitarr[depths];
	       break;
	    }
	    ++depth;
	    atag = tag;
	    entry = (((ONE << depth) + (orig_tag & ((ONE << depth) - 1))) - 1) * BASE;
	/* Skipping levels */
	    while ((depth <= MAX_DEPTH) && (*(arr_ptr + entry) > TWO_PWR_N)) {
		++depth;
	        entry = (((ONE << depth) + (orig_tag & ((ONE << depth) - 1))) - 1) * BASE;
	    }
	    if (depth <= MAX_DEPTH) {
	      slot_ptr = arr_ptr + entry;
	      fst = *slot_ptr;
	    }
	 }
       }
     }
     if (t_entries > T)
       break;
   }

   *hitarr = hitarr0;
   postproc();
}
#endif

