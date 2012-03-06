#define __STDC_FORMAT_MACROS

#include "sift_reader.h"

#include <inttypes.h>
#include <cassert>
#include <cstdio>

int main(int argc, char* argv[])
{
   if (argc > 1) {
      Sift::Reader reader(argv[1]);
      xed_decoded_inst_t xed_inst;
      const xed_syntax_enum_t syntax = XED_SYNTAX_ATT;

      Sift::Instruction inst;
      while(reader.Read(inst))
      {
         printf("%016"PRIxPTR" ", inst.sinst->addr);
         char buffer[40];
         xed_format(syntax, &inst.sinst->xed_inst, buffer, sizeof(buffer), inst.sinst->addr);
         printf("%-40s  ", buffer);

         for(int i = 0; i < inst.sinst->size; ++i)
            printf(" %02x", inst.sinst->data[i]);
         printf("\n");

         if (inst.num_addresses > 0) {
            printf("                 -- addr");
            for(int i = 0; i < inst.num_addresses; ++i)
               printf(" %08"PRIxPTR, inst.addresses[i]);
            printf("\n");
         }
         if (inst.is_branch)
            printf("                 -- %s\n", inst.taken ? "taken" : "not taken");
         if (inst.is_predicate)
            printf("                 -- %s\n", inst.executed ? "executed" : "not executed");
      }
   }
}
