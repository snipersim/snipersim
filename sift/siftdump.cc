#define __STDC_FORMAT_MACROS

#include "sift_reader.h"

#include <inttypes.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#if PIN_REV >= 67254
extern "C" {
//#include "xed-decoded-inst-api.h"
}
#endif

int main(int argc, char* argv[])
{
   if (argc > 1 && strcmp(argv[1], "-d") == 0)
   {
      Sift::Reader reader(argv[2]);
      //const xed_syntax_enum_t syntax = XED_SYNTAX_ATT;

      uint64_t icount = 0;
      std::map<uint64_t, const Sift::StaticInstruction*> instructions;
      std::unordered_map<uint64_t, uint64_t> icounts;

      Sift::Instruction inst;
      while(reader.Read(inst))
      {
         if ((icount++ & 0xffff) == 0)
            fprintf(stderr, "Reading SIFT trace: %" PRId64 "%%\r", 100 * reader.getPosition() / reader.getLength());

         instructions[inst.sinst->addr] = inst.sinst;
         icounts[inst.sinst->addr]++;
      }
      fprintf(stderr, "                                       \r");

      uint64_t eip_last = 0;
      for(auto it = instructions.begin(); it != instructions.end(); ++it)
      {
         if (eip_last && (it->first != eip_last))
            printf("\n");
         printf("%12" PRId64 "   ", icounts[it->first]);
         printf("%16" PRIx64 ":   ", it->first);
         for(int i = 0; i < (it->second->size < 8 ? it->second->size : 8); ++i)
            printf("%02x ", it->second->data[i]);
         for(int i = it->second->size; i < 8; ++i)
            printf("   ");
         char buffer[64] = {0};
#if PIN_REV >= 67254
         //xed_format_context(syntax, &it->second->xed_inst, buffer, sizeof(buffer) - 1, it->first, 0, 0);
#else
         //xed_format(syntax, &it->second->xed_inst, buffer, sizeof(buffer) - 1, it->first);
#endif
         printf("  %-40s\n", buffer);
         if (it->second->size > 8)
         {
            printf("                                   ");
            for(int i = 8; i < it->second->size; ++i)
               printf("%02x ", it->second->data[i]);
            printf("\n");
         }
         eip_last = it->first + it->second->size;
      }
   }
   else if (argc > 1)
   {
      Sift::Reader reader(argv[1]);
      //const xed_syntax_enum_t syntax = XED_SYNTAX_ATT;

      Sift::Instruction inst;
      while(reader.Read(inst))
      {
         printf("%016" PRIx64 " ", inst.sinst->addr);
         char buffer[64] = {0};
#if PIN_REV >= 67254
         //xed_format_context(syntax, &inst.sinst->xed_inst, buffer, sizeof(buffer) - 1, inst.sinst->addr, 0, 0);
#else
         //xed_format(syntax, &inst.sinst->xed_inst, buffer, sizeof(buffer) - 1, inst.sinst->addr);
#endif
         printf("%-40s  ", buffer);

         for(int i = 0; i < inst.sinst->size; ++i)
            printf(" %02x", inst.sinst->data[i]);
         printf("\n");

         if (inst.num_addresses > 0) {
            printf("                 -- addr");
            for(int i = 0; i < inst.num_addresses; ++i)
               printf(" %08" PRIx64, inst.addresses[i]);
            printf("\n");
         }
         if (inst.is_branch)
            printf("                 -- %s\n", inst.taken ? "taken" : "not taken");
         if (inst.is_predicate)
            printf("                 -- %s\n", inst.executed ? "executed" : "not executed");
      }
   }
   else
   {
      printf("Usage: %s [-d] <file.sift>\n", argv[0]);
   }
}
