#ifndef __SIFT_READER_H
#define __SIFT_READER_H

#include "sift.h"

extern "C" {
#include "xed-interface.h"
}

#include <unordered_map>

class vistream;

namespace Sift
{
   // Static information
   typedef struct
   {
      intptr_t addr;
      uint8_t size;
      uint8_t data[16];
      const xed_decoded_inst_t xed_inst;
   } StaticInstruction;

   // Dynamic information
   typedef struct
   {
      const StaticInstruction *sinst;
      uint8_t num_addresses;
      intptr_t addresses[MAX_DYNAMIC_ADDRESSES];
      bool is_branch;
      bool taken;
      bool is_predicate;
      bool executed;
   } Instruction;

   class Reader
   {
      typedef void (*HandleOutputFunc)(void* arg, uint8_t fd, const uint8_t *data, uint32_t size);

      private:
         vistream *input;
         HandleOutputFunc handleOutputFunc;
         void *handleOutputArg;

         static bool xed_initialized;

         intptr_t last_address;
         std::unordered_map<intptr_t, const uint8_t*> icache;
         std::unordered_map<intptr_t, const StaticInstruction*> scache;

         const Sift::StaticInstruction* getStaticInstruction(intptr_t addr, uint8_t size);

      public:
         Reader(const char *filename);
         bool Read(Instruction&);
         void setHandleOutputFunc(HandleOutputFunc func, void* arg = NULL) { handleOutputFunc = func; handleOutputArg = arg; }
   };
};

#endif // __SIFT_READER_H
