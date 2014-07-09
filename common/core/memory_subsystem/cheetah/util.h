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

#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

void fatal(const char* msg);

/* calculates x^y */
int power(int x, int y);

/* creates a two dimensional array dynamically */
uint64_t **idim2(int row, int col);

struct hash_table {
  intptr_t addr;
  int grptime;
  int prty;
  int inum;
  struct hash_table *nxt;
};

void UHT_Add_to_free_list(struct hash_table *free_ptr);
struct hash_table *UHT_Get_from_free_list(void);

struct tree_node {
  intptr_t addr;
  unsigned inum;
  int grpno;
  int prty;
  int rtwt;
  struct tree_node *lft, *rt;
};

/* splay the input entry to the top of the stack */
void splay(int at, struct tree_node **p_stack);

#endif /* UTIL_H */
