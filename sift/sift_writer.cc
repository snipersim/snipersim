#include "sift_writer.h"
#include "sift_format.h"
#include "sift_utils.h"
#include "zfstream.h"

#include <cassert>
#include <cstring>

// Enable to print out everything we write
//#define VERBOSE

Sift::Writer::Writer(std::string filename, GetCodeFunc getCodeFunc, bool useCompression)
   : getCodeFunc(getCodeFunc)
   , ninstrs(0)
   , nbranch(0)
   , npredicate(0)
   , ninstrsmall(0)
   , ninstrext(0)
   , last_address(0)
   , icache()
{
   memset(hsize, 0, sizeof(hsize));
   memset(haddr, 0, sizeof(haddr));

   uint64_t options = 0;
   bool use_z = false;
   if (useCompression)
      options |= Option::CompressionZlib;

   output = new vofstream(filename.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);

   Sift::Header hdr = { Sift::MagicNumber, 0 /* header size */, options};
   output->write(reinterpret_cast<char*>(&hdr), sizeof(hdr));

   if (options & Option::CompressionZlib)
      output = new ozstream(output);
}

Sift::Writer::~Writer()
{
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherEnd;
   rec.Other.size = 0;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));

   delete output;

#if 0
   printf("instrs %lu hsize", ninstrs);
   for(int i = 1; i < 16; ++i)
      printf(" %u", hsize[i]);
   printf(" haddr");
   for(int i = 1; i <= MAX_DYNAMIC_ADDRESSES; ++i)
      printf(" %u", haddr[i]);
   printf(" branch %u predicate %u\n", nbranch, npredicate);
   printf("instrsmall %u ext %u\n", ninstrsmall, ninstrext);
#endif
}

void Sift::Writer::Instruction(intptr_t addr, uint8_t size, uint8_t num_addresses, intptr_t addresses[], bool is_branch, bool taken, bool is_predicate, bool executed)
{
   assert(size < 16);
   assert(num_addresses <= MAX_DYNAMIC_ADDRESSES);

   // Send ICACHE record?
   for(intptr_t base_addr = addr & ICACHE_PAGE_MASK; base_addr <= ((addr + size - 1) & ICACHE_PAGE_MASK); base_addr += ICACHE_SIZE)
   {
      if (! icache[base_addr])
      {
         Record rec;
         rec.Other.zero = 0;
         rec.Other.type = RecOtherIcache;
         rec.Other.size = sizeof(intptr_t) + ICACHE_SIZE;
         output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
         output->write(reinterpret_cast<char*>(&base_addr), sizeof(intptr_t));

         uint8_t buffer[ICACHE_SIZE];
         getCodeFunc(buffer, (const uint8_t *)base_addr, ICACHE_SIZE);
         output->write(reinterpret_cast<char*>(buffer), ICACHE_SIZE);

         icache[base_addr] = true;
      }
   }

   #ifdef VERBOSE
   printf("%016lx (%d) A%u %c%c %c%c\n", addr, size, num_addresses, is_branch?'B':'.', is_branch?(taken?'T':'.'):'.', is_predicate?'C':'.', is_predicate?(executed?'E':'n'):'.');
   #endif

   // Try as simple instruction
   if (addr == last_address && !is_predicate)
   {
      Record rec;
      rec.Instruction.size = size;
      rec.Instruction.num_addresses = num_addresses;
      rec.Instruction.is_branch = is_branch;
      rec.Instruction.taken = taken;
      output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Instruction));

      #ifdef VERBOSE
      hexdump((char*)&rec, sizeof(rec.Instruction));
      #endif

      ninstrsmall++;
   }
   // Send as full instruction
   else
   {
      Record rec;
      memset(&rec, 0, sizeof(rec));
      rec.InstructionExt.type = 0;
      rec.InstructionExt.size = size;
      rec.InstructionExt.num_addresses = num_addresses;
      rec.InstructionExt.is_branch = is_branch;
      rec.InstructionExt.taken = taken;
      rec.InstructionExt.is_predicate = is_predicate;
      rec.InstructionExt.executed = executed;
      rec.InstructionExt.addr = addr;
      output->write(reinterpret_cast<char*>(&rec), sizeof(rec.InstructionExt));

      #ifdef VERBOSE
      hexdump((char*)&rec, sizeof(rec.InstructionExt));
      #endif

      last_address = addr;

      ninstrext++;
   }

   for(int i = 0; i < num_addresses; ++i)
      output->write(reinterpret_cast<char*>(&addresses[i]), sizeof(intptr_t));

   last_address += size;

   ninstrs++;
   hsize[size]++;
   haddr[num_addresses]++;
   if (is_branch)
      nbranch++;
   if (is_predicate)
      npredicate++;
}

void Sift::Writer::Output(uint8_t fd, const char *data, uint32_t size)
{
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherOutput;
   rec.Other.size = sizeof(uint8_t) + size;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&fd), sizeof(uint8_t));
   output->write(data, size);
}
