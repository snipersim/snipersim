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

// Enable (>0) to print out everything we write
#define VERBOSE 0
#define VERBOSE_HEX 0
#define VERBOSE_ICACHE 0


// Weakly-linked default implementation of sift_assert() failure handler
__attribute__ ((weak)) void
__sift_assert_fail(__const char *__assertion, __const char *__file,
                   unsigned int __line, __const char *__function)
       __THROW
{
   __assert_fail(__assertion, __file, __line, __function);
}


Sift::Writer::Writer(const char *filename, GetCodeFunc getCodeFunc, bool useCompression, const char *response_filename, uint32_t id, bool arch32, bool requires_icache_per_insn, bool send_va2pa_mapping)
   : response(NULL)
   , getCodeFunc(getCodeFunc)
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
   if (useCompression)
      options |= CompressionZlib;
   if (arch32)
      options |= ArchIA32;
   if (requires_icache_per_insn)
      options |= IcacheVariable;
   if (m_send_va2pa_mapping)
      options |= PhysicalAddress;

   output = new vofstream(filename, std::ios::out | std::ios::binary | std::ios::trunc);

   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write Header" << std::endl;
   #endif

   Sift::Header hdr = { Sift::MagicNumber, 0 /* header size */, options, {}};
   output->write(reinterpret_cast<char*>(&hdr), sizeof(hdr));
   output->flush();

   if (options & CompressionZlib)
      output = new ozstream(output);
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
         getCodeFunc(buffer, reinterpret_cast<const uint8_t *>(addr), size);
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
            getCodeFunc(buffer, (const uint8_t *)base_addr, ICACHE_SIZE);
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

void Sift::Writer::Output(uint8_t fd, const char *data, uint32_t size)
{
   #if VERBOSE > 1
   std::cerr << "[DEBUG:" << m_id << "] Write Output" << std::endl;
   #endif

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

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherNewThread;
   rec.Other.size = 0;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->flush();
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write NewThread Done" << std::endl;
   #endif

   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new std::ifstream(m_response_filename, std::ios::in);
     sift_assert(!response->fail());
   }

   int32_t retcode = 0;
   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      sift_assert(respRec.Other.zero == 0);

      switch(respRec.Other.type)
      {
         case RecOtherNewThreadResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read NewThreadResponse" << std::endl;
            #endif
            sift_assert(respRec.Other.size == sizeof(retcode));
            response->read(reinterpret_cast<char*>(&retcode), sizeof(retcode));
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Got NewThreadResponse thread=" << retcode << std::endl;
            #endif
            return retcode;
            break;
         default:
            sift_assert(false);
            break;
      }
   }
   return -1;
}

uint64_t Sift::Writer::Syscall(uint16_t syscall_number, const char *data, uint32_t size)
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write Syscall" << std::endl;
   #endif

   // Try to send some extra logical2physical address mappings for data referenced by system call arguments.
   // Also try to read from the address first, if the mapping wasn't set up yet (never accessed before, or swapped out),
   // then this will cause a page fault that brings in the data.
   intptr_t *args = (intptr_t*)data;
   switch(syscall_number)
   {
      case SYS_futex:
      {
         int value = *(int *)args[0];
         send_va2pa(args[0]);
         break;
      }

      case SYS_write:
      {
         int value = *(int *)args[1];
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


   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new std::ifstream(m_response_filename, std::ios::in);
     sift_assert(!response->fail());
   }

   uint64_t retcode = 0;
   while (true)
   {
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      sift_assert(!response->fail());
      sift_assert(respRec.Other.zero == 0);

      switch(respRec.Other.type)
      {
         case RecOtherSyscallResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read SyscallResponse" << std::endl;
            #endif
            sift_assert(respRec.Other.size == sizeof(retcode));
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

   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new std::ifstream(m_response_filename, std::ios::in);
   }

   int32_t retcode = 0;
   while (true)
   {
      #if VERBOSE > 0
      std::cerr << "[DEBUG:" << m_id << "] Join Waiting for Response" << std::endl;
      #endif
      Record respRec;
      response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
      sift_assert(respRec.Other.zero == 0);

      switch(respRec.Other.type)
      {
         case RecOtherJoinResponse:
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Read JoinResponse" << std::endl;
            #endif
            sift_assert(respRec.Other.size == sizeof(retcode));
            response->read(reinterpret_cast<char*>(&retcode), sizeof(retcode));
            return retcode;
            break;
         default:
            sift_assert(false);
            break;
      }
   }
   return -1;
}

void Sift::Writer::Sync()
{
   // send sync
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherSync;
   rec.Other.size = 0;
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->flush();

   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new std::ifstream(m_response_filename, std::ios::in);
   }

   // wait for reply
   Record respRec;
   response->read(reinterpret_cast<char*>(&respRec), sizeof(rec.Other));
   sift_assert(respRec.Other.zero == 0);
   sift_assert(respRec.Other.type == RecOtherSyncResponse);
   sift_assert(respRec.Other.size == 0);
}

uint64_t Sift::Writer::Magic(uint64_t a, uint64_t b, uint64_t c)
{
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

   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new std::ifstream(m_response_filename, std::ios::in);
   }

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

   if (!response)
   {
     sift_assert(strcmp(m_response_filename, "") != 0);
     response = new std::ifstream(m_response_filename, std::ios::in);
   }

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
            sift_assert(false);
            break;
      }
   }

   // We should not get here
   sift_assert(false);
}

void Sift::Writer::RoutineChange(uint64_t eip, uint64_t esp, Sift::RoutineOpType event)
{
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherRoutineChange;
   rec.Other.size = 2*sizeof(uint64_t) + sizeof(uint8_t);
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
   output->write(reinterpret_cast<char*>(&esp), sizeof(uint64_t));
   uint8_t _event = (uint8_t)event;
   output->write(reinterpret_cast<char*>(&_event), sizeof(uint8_t));
}

void Sift::Writer::RoutineAnnounce(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
{
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

void Sift::Writer::handleMemoryRequest(Record &respRec)
{
   #if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Read MemoryRequest" << std::endl;
   #endif
   uint64_t addr;
   uint32_t size;
   MemoryLockType lock;
   MemoryOpType type;
   sift_assert(respRec.Other.size >= (sizeof(addr)+sizeof(size)+sizeof(lock)+sizeof(type)));
   response->read(reinterpret_cast<char*>(&addr), sizeof(addr));
   response->read(reinterpret_cast<char*>(&size), sizeof(size));
   response->read(reinterpret_cast<char*>(&lock), sizeof(lock));
   response->read(reinterpret_cast<char*>(&type), sizeof(type));
   uint32_t payload_size = respRec.Other.size - (sizeof(addr)+sizeof(size)+sizeof(lock)+sizeof(type));
   sift_assert(handleAccessMemoryFunc);
   if (type == MemRead)
   {
      sift_assert(payload_size == 0);
      sift_assert(size > 0);
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
      sift_assert(payload_size > 0);
      sift_assert(payload_size == size);
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
      sift_assert(false);
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

   return pp;
}

void Sift::Writer::send_va2pa(uint64_t va)
{
   if (m_send_va2pa_mapping)
   {
      intptr_t vp = va / PAGE_SIZE;
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
