#include "sift_reader.h"
#include "sift_format.h"
#include "sift_utils.h"
#include "zfstream.h"

#include <iostream>
#include <fstream>
#include <cassert>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

// Enable to print out everything we read
//#define VERBOSE

bool Sift::Reader::xed_initialized = false;

Sift::Reader::Reader(const char *filename)
   : input(NULL)
   , handleOutputFunc(NULL)
   , handleOutputArg(NULL)
   , last_address(0)
   , icache()
{
   if (!xed_initialized)
   {
      xed_tables_init();
      xed_decode_init();
      xed_initialized = true;
   }

   inputstream = new std::ifstream(filename, std::ios::in);

   if (!inputstream->is_open())
   {
      std::cerr << "Cannot open " << filename << std::endl;
      assert(false);
   }

   struct stat filestatus;
   stat(filename, &filestatus);
   filesize = filestatus.st_size;

   input = new vifstream(inputstream);

   Sift::Header hdr;
   input->read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
   assert(hdr.magic == Sift::MagicNumber);
   assert(hdr.size == 0);

   if (hdr.options & Option::CompressionZlib)
   {
      input = new izstream(input);
      hdr.options &= ~Option::CompressionZlib;
   }

   // Make sure there are no unrecognized options
   assert(hdr.options == 0);
}

bool Sift::Reader::Read(Instruction &inst)
{
   while(true)
   {
      Record rec;
      uint8_t byte = input->peek();

      if (byte == 0)
      {
         // Other
         input->read(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
         switch(rec.Other.type)
         {
            case RecOtherEnd:
               return false;
            case RecOtherIcache:
            {
               assert(rec.Other.size == sizeof(intptr_t) + ICACHE_SIZE);
               intptr_t address;
               uint8_t *bytes = new uint8_t[ICACHE_SIZE];
               input->read(reinterpret_cast<char*>(&address), sizeof(intptr_t));
               input->read(reinterpret_cast<char*>(bytes), ICACHE_SIZE);
               icache[address] = bytes;
               break;
            }
            case RecOtherOutput:
            {
               assert(rec.Other.size > sizeof(uint8_t));
               uint8_t fd;
               uint32_t size = rec.Other.size - sizeof(uint8_t);
               uint8_t *bytes = new uint8_t[size];
               input->read(reinterpret_cast<char*>(&fd), sizeof(uint8_t));
               input->read(reinterpret_cast<char*>(bytes), size);
               if (handleOutputFunc)
                  handleOutputFunc(handleOutputArg, fd, bytes, size);
               delete [] bytes;
               break;
            }
            default:
            {
               uint8_t *bytes = new uint8_t[rec.Other.size];
               input->read(reinterpret_cast<char*>(bytes), rec.Other.size);
               delete [] bytes;
               break;
            }
         }
         continue;
      }

      uint8_t size;
      intptr_t addr;

      if ((byte & 0xf) != 0)
      {
         // Instruction
         input->read(reinterpret_cast<char*>(&rec), sizeof(rec.Instruction));

         #ifdef VERBOSE
         hexdump(&rec, sizeof(rec.Instruction));
         #endif

         size = rec.Instruction.size;
         addr = last_address;
         inst.num_addresses = rec.Instruction.num_addresses;
         inst.is_branch = rec.Instruction.is_branch;
         inst.taken = rec.Instruction.taken;
         inst.is_predicate = false;
         inst.executed = true;
      }
      else
      {
         // InstructionExt
         input->read(reinterpret_cast<char*>(&rec), sizeof(rec.InstructionExt));

         #ifdef VERBOSE
         hexdump(&rec, sizeof(rec.InstructionExt));
         #endif

         size = rec.InstructionExt.size;
         addr = rec.InstructionExt.addr;
         inst.num_addresses = rec.InstructionExt.num_addresses;
         inst.is_branch = rec.InstructionExt.is_branch;
         inst.taken = rec.InstructionExt.taken;
         inst.is_predicate = rec.InstructionExt.is_predicate;
         inst.executed = rec.InstructionExt.executed;

         last_address = addr;
      }

      last_address += size;

      for(int i = 0; i < inst.num_addresses; ++i)
         input->read(reinterpret_cast<char*>(&inst.addresses[i]), sizeof(intptr_t));

      inst.sinst = getStaticInstruction(addr, size);

      #ifdef VERBOSE
      hexdump(inst.sinst->data, inst.sinst->size);
      printf("%016lx (%d) A%u %c%c %c%c\n", inst.sinst->addr, inst.sinst->size, inst.num_addresses, inst.is_branch?'B':'.', inst.is_branch?(inst.taken?'T':'.'):'.', inst.is_predicate?'C':'.', inst.is_predicate?(inst.executed?'E':'n'):'.');
      #endif

      return true;
   }
}

const Sift::StaticInstruction* Sift::Reader::getStaticInstruction(intptr_t addr, uint8_t size)
{
   if (!scache.count(addr))
   {
      StaticInstruction *sinst = new StaticInstruction();
      sinst->addr = addr;
      sinst->size = size;

      uint8_t * dst = sinst->data;
      intptr_t base_addr = addr & ICACHE_PAGE_MASK;
      while(size > 0)
      {
         uint32_t offset = (dst == sinst->data) ? addr & ICACHE_OFFSET_MASK : 0;
         uint32_t _size = std::min(uint32_t(size), ICACHE_SIZE - offset);
         memcpy(dst, icache[base_addr] + offset, _size);
         dst += _size;
         size -= _size;
         base_addr += ICACHE_SIZE;
      }

      xed_state_t xed_state = { XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b };
      xed_decoded_inst_zero_set_mode((xed_decoded_inst_t*)&sinst->xed_inst, &xed_state);
      xed_error_enum_t result = xed_decode((xed_decoded_inst_t*)&sinst->xed_inst, sinst->data, sinst->size);
      assert(result == XED_ERROR_NONE);

      scache[addr] = sinst;

   } else
      assert(scache[addr]->size == size);

   return scache[addr];
}

uint64_t Sift::Reader::getPosition()
{
   return inputstream->tellg();
}

uint64_t Sift::Reader::getLength()
{
   return filesize;
}
