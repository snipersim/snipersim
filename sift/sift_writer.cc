#include "fixed_types.h"
#include "sift_writer.h"
#include "sift_format.h"
#include "sift_utils.h"
#include "sift_assert.h"
#include "zfstream.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/param.h>

#ifdef __GNUC__
# include <features.h>
#endif

// Enable (>0) to print out everything we write
#define VERBOSE 0
#define VERBOSE_HEX 0
#define VERBOSE_ICACHE 0

void __assert_fail(const char *__assertion, const char *__file, unsigned int __line, const char *__function) __THROW
{
   std::cerr << "[SIFT] " << __file << ":" << __line << ": " << __function << ": Assertion `" << __assertion << "' failed." << std::endl;
   abort();
}

// Weakly-linked default implementation of sift_assert() failure handler
__attribute__ ((weak)) void
__sift_assert_fail(__const char *__assertion, __const char *__file,
                   unsigned int __line, __const char *__function)
       __THROW
{
   __assert_fail(__assertion, __file, __line, __function);
}


Sift::Writer::Writer(const char *filename, GetCodeFunc getCodeFunc, bool useCompression, const char *response_filename, uint32_t id, bool arch32, bool requires_icache_per_insn, bool send_va2pa_mapping, GetCodeFunc2 getCodeFunc2, void* getCodeFunc2Data)
   : response(NULL)
   , getCodeFunc(getCodeFunc)
   , getCodeFunc2(getCodeFunc2)
   , getCodeFunc2Data(getCodeFunc2Data)
   , ninstrs(0)
   , nbranch(0)
   , npredicate(0)
   , ninstrsmall(0)
   , ninstrext(0)
   , last_address(0)
   , icache()
   , fd_va(-1)
   , m_va2pa()
   , m_id(id)
   , m_requires_icache_per_insn(requires_icache_per_insn)
   , m_send_va2pa_mapping(send_va2pa_mapping)
{
   memset(hsize, 0, sizeof(hsize));
   memset(haddr, 0, sizeof(haddr));

   m_response_filename = strdup(response_filename);

   uint64_t options = 0;
#if SIFT_USE_ZLIB
   if (useCompression)
      options |= CompressionZlib;
#else
   if (useCompression) {
      std::cerr << "[SIFT:" << m_id << "] Warning: Compression disabled, ignoring request.\n";
   }
#endif
   if (arch32)
      options |= ArchIA32;
   if (requires_icache_per_insn)
      options |= IcacheVariable;
   if (m_send_va2pa_mapping)
      options |= PhysicalAddress;

   output = new vofstream(filename, std::ios::out | std::ios::binary | std::ios::trunc);

   if (!output->is_open())
   {
      delete output;
      output = nullptr;
      return;
   }

   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write Header" << std::endl;
   #endif

   #if __GNUC_PREREQ(6,0)
   Sift::Header hdr = { Sift::MagicNumber, 0 /* header size */, options };
   #else
   Sift::Header hdr = { Sift::MagicNumber, 0 /* header size */, options, {} };
   #endif
   output->write(reinterpret_cast<char*>(&hdr), sizeof(hdr));
   output->flush();

   if (options & CompressionZlib)
      output = new ozstream(output);
}

// Modified from http://stackoverflow.com/questions/2203159/is-there-a-c-equivalent-to-getcwd
String get_working_path()
{
   char temp[MAXPATHLEN];
   return ( getcwd(temp, MAXPATHLEN) ? String( temp ) : String("") );
}

void Sift::Writer::initResponse()
{
   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new vifstream(m_response_filename, std::ios::in);
     sift_assert(!response->fail());
   }
}

void Sift::Writer::End()
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write End" << std::endl;
   #endif

   if (output)
   {
      Record rec;
      rec.Other.zero = 0;
      rec.Other.type = RecOtherEnd;
      rec.Other.size = 0;
      output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
      output->flush();
   }

   if (response)
   {
/*
      // Disable EndResponse because of lock-up issues with Pin and sift_recorder
      #if VERBOSE > 0
      std::cerr << "[DEBUG:" << m_id << "] Write End - Response Wait" << std::endl;
      #endif

      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(respRec.Other));
      sift_assert(respRec.Other.zero == 0);
      sift_assert(respRec.Other.type == RecOtherEndResponse);
*/
      delete response;
      response = NULL;
   }

   if (output)
   {
      delete output;
      output = NULL;
   }
}

Sift::Writer::~Writer()
{
   End();

   delete m_response_filename;

   #if VERBOSE > 3
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

void Sift::Writer::Instruction(uint64_t addr, uint8_t size, uint8_t num_addresses, uint64_t addresses[], bool is_branch, bool taken, bool is_predicate, bool executed)
{
   sift_assert(size < 16);
   sift_assert(num_addresses <= MAX_DYNAMIC_ADDRESSES);

   if (!output)
   {
      return;
   }

   if (m_requires_icache_per_insn)
   {
      if (! icache[addr])
      {
         #if VERBOSE_ICACHE
         std::cerr << "[DEBUG:" << m_id << "] Write icache per instruction addr=0x" << std::hex << addr << std::dec << std::endl;
         #endif
         Record rec;
         rec.Other.zero = 0;
         rec.Other.type = RecOtherIcacheVariable;
         rec.Other.size = sizeof(uint64_t) + size;
         output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
         output->write(reinterpret_cast<char*>(&addr), sizeof(uint64_t));

         uint8_t buffer[16] = {0};
         if (getCodeFunc2) {
            getCodeFunc2(buffer, reinterpret_cast<const uint8_t *>(addr), size, getCodeFunc2Data);
         } else {
            getCodeFunc(buffer, reinterpret_cast<const uint8_t *>(addr), size);
         }
         output->write(reinterpret_cast<char*>(buffer), size);

         #if VERBOSE_ICACHE
         hexdump((char*)buffer, sizeof(buffer));
         #endif

         icache[addr] = true;
      }
   }
   else
   {
      // Send ICACHE record?
      for(uint64_t base_addr = addr & ICACHE_PAGE_MASK; base_addr <= ((addr + size - 1) & ICACHE_PAGE_MASK); base_addr += ICACHE_SIZE)
      {
         if (! icache[base_addr])
         {
            #if VERBOSE > 2
            std::cerr << "[DEBUG:" << m_id << "] Write icache" << std::endl;
            #endif
            Record rec;
            rec.Other.zero = 0;
            rec.Other.type = RecOtherIcache;
            rec.Other.size = sizeof(uint64_t) + ICACHE_SIZE;
            output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
            output->write(reinterpret_cast<char*>(&base_addr), sizeof(uint64_t));

            uint8_t buffer[ICACHE_SIZE];
            if (getCodeFunc2) {
               getCodeFunc2(buffer, (const uint8_t *)base_addr, ICACHE_SIZE, getCodeFunc2Data);
            } else {
               getCodeFunc(buffer, (const uint8_t *)base_addr, ICACHE_SIZE);
            }
            output->write(reinterpret_cast<char*>(buffer), ICACHE_SIZE);

            icache[base_addr] = true;
         }
      }
   }

   #if VERBOSE > 2
   printf("%016lx (%d) A%u %c%c %c%c\n", addr, size, num_addresses, is_branch?'B':'.', is_branch?(taken?'T':'.'):'.', is_predicate?'C':'.', is_predicate?(executed?'E':'n'):'.');
   #endif

   send_va2pa(addr);
   for(int i = 0; i < num_addresses; ++i)
      send_va2pa(addresses[i]);

   // Try as simple instruction
   if (addr == last_address && !is_predicate)
   {
      #if VERBOSE > 2
      std::cerr << "[DEBUG:" << m_id << "] Write Simple Instruction" << std::endl;
      #endif

      Record rec;
      rec.Instruction.size = size;
      rec.Instruction.num_addresses = num_addresses;
      rec.Instruction.is_branch = is_branch;
      rec.Instruction.taken = taken;
      output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Instruction));

      #if VERBOSE_HEX > 2
      hexdump((char*)&rec, sizeof(rec.Instruction));
      #endif

      ninstrsmall++;
   }
   // Send as full instruction
   else
   {
      #if VERBOSE > 2
      std::cerr << "[DEBUG:" << m_id << "] Write Simple Full Instruction" << std::endl;
      #endif

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

      #if VERBOSE_HEX > 2
      hexdump((char*)&rec, sizeof(rec.InstructionExt));
      #endif

      last_address = addr;

      ninstrext++;
   }

   for(int i = 0; i < num_addresses; ++i)
      output->write(reinterpret_cast<char*>(&addresses[i]), sizeof(uint64_t));

   last_address += size;

   ninstrs++;
   hsize[size]++;
   haddr[num_addresses]++;
   if (is_branch)
      nbranch++;
   if (is_predicate)
      npredicate++;
}

Sift::Mode Sift::Writer::InstructionCount(uint32_t icount)
{
   #if VERBOSE > 1
   std::cerr << "[DEBUG:" << m_id << "] Write InstructionCount" << std::endl;
   #endif

   if (!output)
   {
      return Sift::ModeUnknown;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherInstructionCount;
   rec.Other.size = sizeof(uint32_t);

   #if VERBOSE_HEX > 1
   hexdump((char*)&rec, sizeof(rec.Other));
   hexdump((char*)&icount, sizeof(icount));
   #endif

   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&icount), sizeof(icount));
   output->flush();

   initResponse();

   // wait for reply
   Record respRec;
   response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
   if (respRec.Other.zero != 0)
   {
      return Sift::ModeUnknown;
   }
   if (respRec.Other.type != RecOtherSyncResponse)
   {
      return Sift::ModeUnknown;
   }
   Mode mode;
   if (respRec.Other.size != sizeof(Mode))
   {
      return Sift::ModeUnknown;
   }
   response->read(reinterpret_cast<char*>(&mode), sizeof(Mode));
   return mode;
}

void Sift::Writer::CacheOnly(uint8_t icount, CacheOnlyType type, uint64_t eip, uint64_t address)
{
   #if VERBOSE > 1
   std::cerr << "[DEBUG:" << m_id << "] Write CacheOnly" << std::endl;
   #endif

   if (!output)
   {
      return;
   }

   send_va2pa(eip);
   send_va2pa(address);

   uint8_t _type = type;

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherCacheOnly;
   rec.Other.size = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint64_t);

   #if VERBOSE_HEX > 1
   hexdump((char*)&rec, sizeof(rec.Other));
   hexdump((char*)&icount, sizeof(icount));
   hexdump((char*)&_type, sizeof(_type));
   hexdump((char*)&eip, sizeof(eip));
   hexdump((char*)&address, sizeof(address));
   #endif

   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&icount), sizeof(uint8_t));
   output->write(reinterpret_cast<char*>(&_type), sizeof(uint8_t));
   output->write(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&address), sizeof(uint64_t));
}

void Sift::Writer::Output(uint8_t fd, const char *data, uint32_t size)
{
   #if VERBOSE > 1
   std::cerr << "[DEBUG:" << m_id << "] Write Output" << std::endl;
   #endif

   if (!output)
   {
      return;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherOutput;
   rec.Other.size = sizeof(uint8_t) + size;

   #if VERBOSE_HEX > 1
   hexdump((char*)&rec, sizeof(rec.Other));
   hexdump((char*)&fd, sizeof(fd));
   hexdump((char*)data, size);
   #endif

   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&fd), sizeof(uint8_t));
   output->write(data, size);
}

int32_t Sift::Writer::NewThread()
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write NewThread" << std::endl;
   #endif

   if (!output)
   {
      return -1;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherNewThread;
   rec.Other.size = 0;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->flush();
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write NewThread Done" << std::endl;
   #endif

   initResponse();

   int32_t retcode = 0;
   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      if (respRec.Other.zero != 0)
      {
         return -1;
      }

      switch(respRec.Other.type)
      {
         case RecOtherNewThreadResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read NewThreadResponse" << std::endl;
            #endif
            if (respRec.Other.size != sizeof(retcode))
            {
               return -1;
            }
            response->read(reinterpret_cast<char*>(&retcode), sizeof(retcode));
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Got NewThreadResponse thread=" << retcode << std::endl;
            #endif
            return retcode;
            break;
         default:
            return -1;
            break;
      }
   }
   return -1;
}

template<typename T>
static T force_read(volatile T *addr)
{
    return *addr;
}

uint64_t Sift::Writer::Syscall(uint16_t syscall_number, const char *data, uint32_t size)
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write Syscall" << std::endl;
   #endif

   if (!output)
   {
      return 1;
   }

   // Try to send some extra logical2physical address mappings for data referenced by system call arguments.
   // Also try to read from the address first, if the mapping wasn't set up yet (never accessed before, or swapped out),
   // then this will cause a page fault that brings in the data.
   intptr_t *args = (intptr_t*)data;
   switch(syscall_number)
   {
      case SYS_futex:
      {
         force_read(reinterpret_cast<int*>(args[0]));
         send_va2pa(args[0]);
         break;
      }

      case SYS_write:
      {
         force_read(reinterpret_cast<int*>(args[1]));
         send_va2pa(args[1]);
         break;
      }
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherSyscallRequest;
   rec.Other.size = sizeof(uint16_t) + size;
   #if VERBOSE_HEX > 0
   hexdump((char*)&rec, sizeof(rec.Other));
   hexdump((char*)&syscall_number, sizeof(syscall_number));
   hexdump((char*)data, size);
   #endif
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&syscall_number), sizeof(uint16_t));
   output->write(data, size);
   output->flush();

   initResponse();

   uint64_t retcode = 0;
   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      if (response->fail())
      {
         return 1;
      }
      if (respRec.Other.zero != 0)
      {
         return 1;
      }

      switch(respRec.Other.type)
      {
         case RecOtherSyscallResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read SyscallResponse" << std::endl;
            #endif
            if (respRec.Other.size != sizeof(retcode))
            {
               return 1;
            }
            response->read(reinterpret_cast<char*>(&retcode), sizeof(retcode));
            return retcode;
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            break;
      }
   }

   // We should not get here
   sift_assert(false);
}

int32_t Sift::Writer::Join(int32_t thread)
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write Join with thread=" << thread << std::endl;
   #endif

   if (!output)
   {
      return -1;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherJoin;
   rec.Other.size = sizeof(thread);
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&thread), sizeof(thread));
   output->flush();
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write Join Done" << std::endl;
   #endif

   initResponse();

   int32_t retcode = 0;
   while (true)
   {
      #if VERBOSE > 0
      std::cerr << "[DEBUG:" << m_id << "] Join Waiting for Response" << std::endl;
      #endif
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      if (respRec.Other.zero != 0)
      {
         return -1;
      }

      switch(respRec.Other.type)
      {
         case RecOtherJoinResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read JoinResponse" << std::endl;
            #endif
            if (respRec.Other.size != sizeof(retcode))
            {
               return -1;
            }
            response->read(reinterpret_cast<char*>(&retcode), sizeof(retcode));
            return retcode;
            break;
         default:
            return -1;
            break;
      }
   }
   return -1;
}

Sift::Mode Sift::Writer::Sync()
{
   if (!output)
   {
      return Sift::ModeUnknown;
   }

   // send sync
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherSync;
   rec.Other.size = 0;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->flush();

   initResponse();

   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      if (response->fail())
      {
         return Sift::ModeUnknown;
      }
      if (respRec.Other.zero != 0)
      {
         return Sift::ModeUnknown;
      }

      switch(respRec.Other.type)
      {
         case RecOtherSyncResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read SyncResponse" << std::endl;
            #endif
            Mode mode;
            if (respRec.Other.size != sizeof(Mode))
            {
               return Sift::ModeUnknown;
            }
            response->read(reinterpret_cast<char*>(&mode), sizeof(Mode));
            return mode;
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            break;
         default:
            return Sift::ModeUnknown;
            break;
      }
   }

   // We should not get here
   sift_assert(false);
}

int32_t Sift::Writer::Fork()
{
   if (!output)
   {
      return -1;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherFork;
   rec.Other.size = 0;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->flush();

   initResponse();

   Record respRec;
   response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));

   if (response->fail())
   {
      return -1;
   }
   if (respRec.Other.zero != 0)
   {
      return -1;
   }
   if (respRec.Other.type != RecOtherForkResponse)
   {
      return -1;
   }
   if (respRec.Other.size != sizeof(int32_t))
   {
      return -1;
   }

   int32_t result;
   response->read(reinterpret_cast<char*>(&result), sizeof(int32_t));
   if (response->fail())
   {
      return -1;
   }
   return result;
}

uint64_t Sift::Writer::Magic(uint64_t a, uint64_t b, uint64_t c)
{
   if (!output)
   {
      return 1;
   }

   // send magic
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherMagicInstruction;
   rec.Other.size = 3 * sizeof(uint64_t);
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&a), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&b), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&c), sizeof(uint64_t));
   output->flush();

   initResponse();

   // wait for reply
   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      sift_assert(!response->fail());
      sift_assert(respRec.Other.zero == 0);

      switch(respRec.Other.type)
      {
         case RecOtherMagicInstructionResponse:
         {
            sift_assert(respRec.Other.size == sizeof(uint64_t));
            uint64_t result;
            response->read(reinterpret_cast<char*>(&result), sizeof(uint64_t));
            return result;
         }
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            break;
         default:
            sift_assert(false);
            break;
      }
   }

   // We should not get here
   sift_assert(false);
}

bool Sift::Writer::Emulate(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res)
{
   if (!output)
   {
      return false;
   }

   // send magic
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherEmu;
   rec.Other.size = sizeof(uint16_t) + sizeof(EmuRequest);
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   uint16_t _type = type;
   output->write(reinterpret_cast<char*>(&_type), sizeof(uint16_t));
   output->write(reinterpret_cast<char*>(&req), sizeof(EmuRequest));
   output->flush();

   initResponse();

   // wait for reply
   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      sift_assert(!response->fail());
      sift_assert(respRec.Other.zero == 0);

      switch(respRec.Other.type)
      {
         case RecOtherEmuResponse:
         {
            sift_assert(respRec.Other.size <= sizeof(uint8_t) + sizeof(EmuReply));
            uint8_t result;
            response->read(reinterpret_cast<char*>(&result), sizeof(uint8_t));
            // Support servers which still use a smaller EmuReply
            response->read(reinterpret_cast<char*>(&res), respRec.Other.size - sizeof(uint8_t));
            return result;
         }
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            break;
         default:
            return false;
            break;
      }
   }

   // We should not get here
   sift_assert(false);
}

void Sift::Writer::RoutineChange(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip)
{
   if (!output)
   {
      return;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherRoutineChange;
   rec.Other.size = sizeof(uint8_t) + 3 * sizeof(uint64_t);
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   uint8_t _event = (uint8_t)event;
   output->write(reinterpret_cast<char*>(&_event), sizeof(uint8_t));
   output->write(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&esp), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&callEip), sizeof(uint64_t));
}

void Sift::Writer::RoutineAnnounce(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
{
   if (!output)
   {
      return;
   }

   uint16_t len_name = strlen(name) + 1, len_imgname = strlen(imgname) + 1, len_filename = strlen(filename) + 1;

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherRoutineAnnounce;
   rec.Other.size = sizeof(uint64_t) + sizeof(uint16_t) + len_name + sizeof(uint16_t) + len_imgname + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t) + len_filename;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&len_name), sizeof(uint16_t));
   output->write(name, len_name);
   output->write(reinterpret_cast<char*>(&len_imgname), sizeof(uint16_t));
   output->write(imgname, len_imgname);
   output->write(reinterpret_cast<char*>(&offset), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&line), sizeof(uint32_t));
   output->write(reinterpret_cast<char*>(&column), sizeof(uint32_t));
   output->write(reinterpret_cast<char*>(&len_filename), sizeof(uint16_t));
   output->write(filename, len_filename);
}

void Sift::Writer::ISAChange(uint32_t new_isa)
{
   #if VERBOSE > 1
   std::cerr << "[DEBUG:" << m_id << "] Write ISAChange" << std::endl;
   #endif

   if (!output)
   {
      return;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherISAChange;
   rec.Other.size = sizeof(uint32_t);

   #if VERBOSE_HEX > 1
   hexdump((char*)&rec, sizeof(rec.Other));
   hexdump((char*)&new_isa, sizeof(new_isa));
   #endif

   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&new_isa), sizeof(new_isa));
}

bool Sift::Writer::IsOpen()
{
   return !!output;
}

void Sift::Writer::handleMemoryRequest(Record &respRec)
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Read MemoryRequest" << std::endl;
   #endif

   if (!output)
   {
      return;
   }

   uint64_t addr;
   uint32_t size;
   MemoryLockType lock;
   MemoryOpType type;
   if (respRec.Other.size < (sizeof(addr)+sizeof(size)+sizeof(lock)+sizeof(type)))
   {
      return;
   }
   response->read(reinterpret_cast<char*>(&addr), sizeof(addr));
   response->read(reinterpret_cast<char*>(&size), sizeof(size));
   response->read(reinterpret_cast<char*>(&lock), sizeof(lock));
   response->read(reinterpret_cast<char*>(&type), sizeof(type));
   uint32_t payload_size = respRec.Other.size - (sizeof(addr)+sizeof(size)+sizeof(lock)+sizeof(type));
   if (!handleAccessMemoryFunc)
   {
      return;
   }
   if (type == MemRead)
   {
      if (payload_size != 0)
      {
         return;
      }
      if (size <= 0)
      {
         return;
      }
      char *read_data = new char[size];
      bzero(read_data, size);
      // Do the read here via a callback to populate the read buffer
      handleAccessMemoryFunc(handleAccessMemoryArg, lock, type, addr, (uint8_t*)read_data, size);
      Record rec;
      rec.Other.zero = 0;
      rec.Other.type = RecOtherMemoryResponse;
      rec.Other.size = sizeof(addr) + sizeof(type) + size;
      #if VEBOSE_HEX > 0
      hexdump((char*)&rec, sizeof(rec.Other));
      hexdump((char*)&addr, sizeof(addr));
      hexdump((char*)&type, sizeof(type));
      hexdump((char*)read_data, size);
      #endif
      #if VERBOSE
      std::cerr << "[DEBUG:" << m_id << "] Write AccessMemory-Read" << std::endl;
      #endif

      output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
      output->write(reinterpret_cast<char*>(&addr), sizeof(addr));
      output->write(reinterpret_cast<char*>(&type), sizeof(type));
      output->write(read_data, size);
      output->flush();
      delete [] read_data;
   }
   else if (type == MemWrite)
   {
      #if VERBOSE > 0
      std::cerr << "[DEBUG:" << m_id << "] Write AccessMemory-Write" << std::endl;
      #endif
      if (payload_size <= 0)
      {
         return;
      }
      if (payload_size != size)
      {
         return;
      }
      char *payload = new char[payload_size];
      response->read(reinterpret_cast<char*>(payload), payload_size);
      // Do the write here via a callback to write the data to the appropriate address
      handleAccessMemoryFunc(handleAccessMemoryArg, lock, type, addr, (uint8_t*)payload, payload_size);
      Record rec;
      rec.Other.zero = 0;
      rec.Other.type = RecOtherMemoryResponse;
      rec.Other.size = sizeof(addr) + sizeof(type);
      #if VEBOSE_HEX > 0
      hexdump((char*)&rec, sizeof(rec.Other));
      hexdump((char*)&addr, sizeof(addr));
      hexdump((char*)&type, sizeof(type));
      #endif
      output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
      output->write(reinterpret_cast<char*>(&addr), sizeof(addr));
      output->write(reinterpret_cast<char*>(&type), sizeof(type));
      output->flush();
      delete [] payload;
   }
   else
   {
      return;
   }
}

uint64_t Sift::Writer::va2pa_lookup(uint64_t vp)
{
   // Ignore vsyscall range
   if (vp >= 0xffffffffff600ULL && vp < 0xfffffffffffffULL)
      return vp;

   if (fd_va == -1)
   {
      fd_va = open("/proc/self/pagemap", O_RDONLY);
      if (fd_va == -1)
      {
         perror("Cannot open /proc/self/pagemap");
         exit(1);
      }
   }
   off64_t index = vp * sizeof(intptr_t);
   off64_t offset = lseek64(fd_va, index, SEEK_SET);
   sift_assert(offset == index);
   intptr_t pp;
   ssize_t size = read(fd_va, &pp, sizeof(intptr_t));

   if (size != sizeof(intptr_t))
   {
      // Lookup failed. This happens for [vdso] sections.
      return vp;
   }

   // From: https://stackoverflow.com/questions/5748492/is-there-any-api-for-determining-the-physical-address-from-virtual-address-in-li
   // From: https://stackoverflow.com/a/45128487

   intptr_t pfn = pp & (((intptr_t)1 << 54) - 1);
   bool present = (pp >> 63) & 1;

   if (!present)
   {
      // Lookup failed, Use pfn == vp
      return vp;
   }

   // From: https://www.kernel.org/doc/Documentation/vm/pagemap.txt
   // Since kernel 4.2, access to the pagemap is restricted, and the pfn is zeroed
   // Check for present and pfn == 0 to see if we have permission to run here

   // A zero-valued pfn is suspicious
   sift_assert(pfn != 0);

   return pfn;
}

void Sift::Writer::send_va2pa(uint64_t va)
{
   if (!output)
   {
      return;
   }

   if (m_send_va2pa_mapping)
   {
      uint64_t vp = static_cast<uintptr_t>(va) / PAGE_SIZE_SIFT;
      if (m_va2pa.count(vp) == 0)
      {
         uint64_t pp = va2pa_lookup(vp);
         if (pp == 0)
         {
            // Page is not mapped into physical memory, do not send a mapping now.
         }
         else
         {
            Record rec;
            rec.Other.zero = 0;
            rec.Other.type = RecOtherLogical2Physical;
            rec.Other.size = 2 * sizeof(uint64_t);
            output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
            output->write(reinterpret_cast<char*>(&vp), sizeof(uint64_t));
            output->write(reinterpret_cast<char*>(&pp), sizeof(uint64_t));

            m_va2pa[vp] = true;
         }
      }
   }
}
