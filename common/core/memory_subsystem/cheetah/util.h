
#ifndef UTIL_H
#define UTIL_H

/* calculates x^y */
int power(int x, int y);

/* creates a two dimensional array dynamically */
unsigned **idim2(int row, int col);

struct hash_table {
  md_addr_t addr;
  int grptime;
  int prty;
  int inum;
  struct hash_table *nxt;
};

void UHT_Add_to_free_list(struct hash_table *free_ptr);
struct hash_table *UHT_Get_from_free_list(void);

struct tree_node {
  md_addr_t addr;
  unsigned inum;
  int grpno;
  int prty;
  int rtwt;
  struct tree_node *lft, *rt;
};  

/* splay the input entry to the top of the stack */
void splay(int at, struct tree_node **p_stack);

#endif /* UTIL_H */
