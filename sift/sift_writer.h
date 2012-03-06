#ifndef __SIFT_WRITER_H
#define __SIFT_WRITER_H

#include "sift.h"

#include <unordered_map>

class vostream;

namespace Sift
{
   class Writer
   {
      typedef void (*GetCodeFunc)(uint8_t *dst, const uint8_t *src, uint32_t size);

      private:
         vostream *output;
         GetCodeFunc getCodeFunc;
         uint64_t ninstrs, hsize[16], haddr[MAX_DYNAMIC_ADDRESSES+1], nbranch, npredicate, ninstrsmall, ninstrext;

         intptr_t last_address;
         std::unordered_map<intptr_t, bool> icache;
      public:
         Writer(std::string filename, GetCodeFunc getCodeFunc, bool useCompression = true);
         ~Writer();
         void Instruction(intptr_t addr, uint8_t size, uint8_t num_addresses, intptr_t addresses[], bool is_branch, bool taken, bool is_predicate, bool executed);
         void Output(uint8_t fd, const char *data, uint32_t size);
   };
};

#endif // __SIFT_WRITER_H
