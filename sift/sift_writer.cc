// off64_t and lseek64 are used below for /proc/self/pagemap, whose offsets run
// past 2GB.  glibc also exposes them under _GNU_SOURCE, which g++ defines on its
// own, so the host build never had to ask; PinCRT looks only at this one.  It
// has to precede every header.
#define _LARGEFILE64_SOURCE 1

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
#include <unistd.h>   // getcwd, read, lseek64 -- previously leaked in by the host headers

#ifdef __GNUC__
# include <features.h>
#endif

// Enable (>0) to print out everything we write
#define VERBOSE 0
#define VERBOSE_HEX 0
#define VERBOSE_ICACHE 0

//#define DEBUG_SIFT_WRITER

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


// ---------------------------------------------------------------------------
// CodeTracker (see sift_writer.h).  A spin lock rather than std::mutex: this
// file is also built against PinCRT, whose thread support is not the host's,
// and every operation under the lock is rare (translation, page sends).
namespace
{
   std::atomic_flag s_code_lock = ATOMIC_FLAG_INIT;
   struct CodeLock
   {
      CodeLock() { while (s_code_lock.test_and_set(std::memory_order_acquire)) ; }
      ~CodeLock() { s_code_lock.clear(std::memory_order_release); }
   };
   // The bytes of each code page as last sent (or since observed to change)
   std::unordered_map<uint64_t, std::array<uint8_t, Sift::ICACHE_SIZE> > *s_code_pages = NULL;
   // Pages logged as changed, in order; s_changes is its length
   std::vector<uint64_t> *s_code_log = NULL;

   void codeInit()
   {
      if (!s_code_pages)
      {
         s_code_pages = new std::unordered_map<uint64_t, std::array<uint8_t, Sift::ICACHE_SIZE> >();
         s_code_log = new std::vector<uint64_t>();
      }
   }
}

bool Sift::CodeTracker::s_enabled = false;
std::atomic<uint64_t> Sift::CodeTracker::s_changes(0);

void Sift::CodeTracker::observe(uint64_t addr, const uint8_t *bytes, uint32_t size)
{
   if (!s_enabled || size == 0)
      return;
   CodeLock lock;
   codeInit();
   uint64_t a = addr, end = addr + size;
   while (a < end)
   {
      uint64_t base = a & ICACHE_PAGE_MASK;
      uint64_t offset = a & ICACHE_OFFSET_MASK;
      uint64_t n = std::min<uint64_t>(end - a, ICACHE_SIZE - offset);
      auto it = s_code_pages->find(base);
      // A page nobody has sent yet needs nothing: it will be sent as it is
      if (it != s_code_pages->end() && memcmp(&it->second[offset], bytes + (a - addr), n) != 0)
      {
         // Keep the snapshot current, so that the writers' re-sends of this
         // page compare equal and do not log it a second time
         memcpy(&it->second[offset], bytes + (a - addr), n);
         s_code_log->push_back(base);
         s_changes.store(s_code_log->size(), std::memory_order_release);
      }
      a += n;
   }
}

void Sift::CodeTracker::changedPages(uint64_t from, uint64_t to, std::vector<uint64_t> &pages)
{
   CodeLock lock;
   for (uint64_t i = from; i < to && s_code_log && i < s_code_log->size(); ++i)
      pages.push_back((*s_code_log)[i]);
}

uint64_t Sift::CodeTracker::pageSent(uint64_t base, const uint8_t *bytes, uint64_t seen)
{
   CodeLock lock;
   codeInit();
   auto it = s_code_pages->find(base);
   if (it == s_code_pages->end())
   {
      std::array<uint8_t, ICACHE_SIZE> &page = (*s_code_pages)[base];
      memcpy(page.data(), bytes, ICACHE_SIZE);
      return seen;
   }
   if (memcmp(it->second.data(), bytes, ICACHE_SIZE) == 0)
      return seen;
   // Changed since another writer sent it, and not yet observed: the other
   // writers' copies are stale
   memcpy(it->second.data(), bytes, ICACHE_SIZE);
   bool caught_up = (s_code_log->size() == seen);
   s_code_log->push_back(base);
   s_changes.store(s_code_log->size(), std::memory_order_release);
   return caught_up ? seen + 1 : seen;
}

uint64_t Sift::CodeTracker::pagesTracked()
{
   CodeLock lock;
   return s_code_pages ? s_code_pages->size() : 0;
}
// ---------------------------------------------------------------------------

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
   // A new writer has sent nothing yet, so no earlier change concerns it
   , m_code_changes_seen(CodeTracker::changes())
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

   output = new cvofstream(filename);      // a FIFO open that does not wait inside stdio (zfstream.h)

   if (!output->is_open())
   {
      std::cerr << "[SIFT:" << m_id << "] Error: could not open " << filename << " for writing; no trace will be produced" << std::endl;
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
     //response = new vifstream(m_response_filename, std::ios::in);
     response = new cvifstream(m_response_filename, std::ios_base::in);
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

   // free(), not delete: m_response_filename comes from strdup(), which
   // allocates with malloc().  Freeing it with operator delete is undefined,
   // and not harmlessly so here -- the recorder is built against PinCRT, whose
   // allocator is not the one strdup() used, and the mismatch showed up at
   // every shutdown as
   //
   //     free(): invalid pointer
   //
   // followed by glibc calling abort().  Under MPI that abort was fatal to the
   // whole job: the process manager saw a rank die abnormally, killed the rest
   // with SIGKILL, and every other rank's SIFT pipe broke mid-stream.
   //
   // sift_reader.cc pairs its own strdup()s with free() correctly; only the
   // writer was mismatched.
   free(m_response_filename);

   #if VERBOSE > 3
   printf("instrs %lu hsize", ninstrs);
   for(int i = 1; i < 16; ++i)
      printf(" %lu", hsize[i]);
   printf(" haddr");
   for(unsigned i = 1; i <= MAX_DYNAMIC_ADDRESSES; ++i)
      printf(" %lu", haddr[i]);
   printf(" branch %lu predicate %lu\n", nbranch, npredicate);
   printf("instrsmall %lu ext %lu\n", ninstrsmall, ninstrext);
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
      // Fetch first, then decide: with self-modifying code the bytes at an
      // address change under us, so "have we sent this address" is the wrong
      // question.  The right one is "are these the bytes we last sent".
      uint8_t current[16] = {0};
      if (getCodeFunc2) {
         getCodeFunc2(current, reinterpret_cast<const uint8_t *>(addr), size, getCodeFunc2Data);
      } else {
         getCodeFunc(current, reinterpret_cast<const uint8_t *>(addr), size);
      }

      bool changed = true;
      std::unordered_map<uint64_t, std::array<uint8_t, 16> >::const_iterator seen =
         icache_bytes.find(addr);
      if (seen != icache_bytes.end())
         changed = (memcmp(seen->second.data(), current, size) != 0);

      if (changed)
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

         output->write(reinterpret_cast<char*>(current), size);

         #if VERBOSE_ICACHE
         hexdump((char*)current, sizeof(current));
         #endif

         std::array<uint8_t, 16> keep;
         memcpy(keep.data(), current, sizeof(current));
         icache_bytes[addr] = keep;
      }
   }
   else
   {
      // Code changed since pages were sent (CodeTracker): forget the changed
      // pages, so that they are sent again below with their current bytes.
      // The common case is one relaxed comparison per instruction.
      if (CodeTracker::enabled())
      {
         uint64_t changes = CodeTracker::changes();
         if (changes != m_code_changes_seen)
         {
            std::vector<uint64_t> pages;
            CodeTracker::changedPages(m_code_changes_seen, changes, pages);
            for (uint64_t page : pages)
               icache.erase(page);
            m_code_changes_seen = changes;
         }
      }

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
            if (CodeTracker::enabled())
               m_code_changes_seen = CodeTracker::pageSent(base_addr, buffer, m_code_changes_seen);
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
   if (response->fail())
   {
      channelLost();
      return Sift::ModeUnknown;
   }
   if (respRec.Other.zero != 0)
   {
      return Sift::ModeUnknown;
   }
   if (respRec.Other.type != RecOtherSyncResponse)
   {
	if (respRec.Other.type == RecOtherShutdown)
		frontEndStop();
      return Sift::ModeUnknown;
   }
   Mode mode;
   if (respRec.Other.size != sizeof(Mode))
   {
      return Sift::ModeUnknown;
   }
   response->read(reinterpret_cast<char*>(&mode), sizeof(Mode));
   if (response->fail())
   {
      channelLost();
      return Sift::ModeUnknown;
   }
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
      if (response->fail())
      {
         channelLost();
         return -1;
      }
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
            if (response->fail())
            {
               channelLost();
               return -1;
            }
            #if VERBOSE > 0
            std::cerr << "[DEBUG:" << m_id << "] Got NewThreadResponse thread=" << retcode << std::endl;
            #endif
            return retcode;
            break;
	 case RecOtherShutdown:
	    frontEndStop();
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
         channelLost();
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
            if (response->fail())
            {
               channelLost();
               return 1;
            }
            return retcode;
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            if (!response)
               return 1;
            break;
	 case RecOtherShutdown:
	    frontEndStop();
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
      if (response->fail())
      {
         channelLost();
         return -1;
      }
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
            if (response->fail())
            {
               channelLost();
               return -1;
            }
            return retcode;
            break;
	 case RecOtherShutdown:
	    frontEndStop();
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
         channelLost();
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
            if (response->fail())
            {
               channelLost();
               return Sift::ModeUnknown;
            }
            return mode;
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            if (!response)
               return Sift::ModeUnknown;
            break;
	 case RecOtherShutdown:
	    frontEndStop();
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
      channelLost();
      return -1;
   }

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
      if (respRec.Other.type == RecOtherShutdown)
	frontEndStop();
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
      channelLost();
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
      if (response->fail())
      {
         channelLost();
         return 0;
      }
      // printf("Record type: %" PRIu8 "\n", respRec.Other.type);
      // printf("Record zero: %" PRIu8 "\n", respRec.Other.zero);


      sift_assert(!response->fail());
      sift_assert(respRec.Other.zero == 0);

      switch(respRec.Other.type)
      {
         case RecOtherMagicInstructionResponse:
         {
            sift_assert(respRec.Other.size == sizeof(uint64_t));
            uint64_t ack;
            response->read(reinterpret_cast<char*>(&ack), sizeof(uint64_t));
            if (response->fail())
            {
               channelLost();
               return 0;
            }

            
            return ack; // 0 - no error; 1 - otherwise
         }
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            if (!response)
               return 0;
            break;
	 case RecOtherShutdown:
	    frontEndStop();
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
            if (response->fail())
            {
               channelLost();
               return false;
            }
            // Support servers which still use a smaller EmuReply
            response->read(reinterpret_cast<char*>(&res), respRec.Other.size - sizeof(uint8_t));
            if (response->fail())
            {
               channelLost();
               return false;
            }
            return result;
         }
         case RecOtherMemoryRequest:
            handleMemoryRequest(respRec);
            if (!response)
               return false;
            break;
	 case RecOtherShutdown:
	    frontEndStop();
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

void Sift::Writer::MemorySpans(uint8_t count, const MemorySpan spans[], uint32_t dropped_lines)
{
   /* count spans, then the spans, then the number of cache lines the
      instruction touched beyond them (an access wider than the record holds:
      a gather of thousands of lines at a wide VLEN).  A reader that predates
      the count accepts the record without it. */
   if (!output)
      return;
   sift_assert(count <= MAX_MEMORY_SPANS);
   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherMemorySpans;
   rec.Other.size = sizeof(uint8_t) + count * sizeof(MemorySpan) + sizeof(dropped_lines);
   output->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   output->write(reinterpret_cast<const char*>(&count), sizeof(count));
   output->write(reinterpret_cast<const char*>(spans), count * sizeof(MemorySpan));
   output->write(reinterpret_cast<const char*>(&dropped_lines), sizeof(dropped_lines));
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
   if (response->fail())
   {
      channelLost();
      return;
   }
   response->read(reinterpret_cast<char*>(&size), sizeof(size));
   if (response->fail())
   {
      channelLost();
      return;
   }
   response->read(reinterpret_cast<char*>(&lock), sizeof(lock));
   if (response->fail())
   {
      channelLost();
      return;
   }
   response->read(reinterpret_cast<char*>(&type), sizeof(type));
   if (response->fail())
   {
      channelLost();
      return;
   }
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
      if (response->fail())
      {
         channelLost();
         return;
      }
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

/* The simulator closed this thread's channel -- its trace thread was stopped
   because the application ended (a sibling's exit_group) and the reader went
   away.  The recorder side runs on natively; every later request must find
   no channel rather than read whatever a closed pipe returns (an
   uninitialised record whose type could be anything, Shutdown included, which
   exited the process from inside an analysis routine) or write into a pipe
   with no reader (SIGPIPE). */
void Sift::Writer::channelLost()
{
   if (output || response)
      std::cerr << "[SIFT:" << m_id << "] channel to the simulator lost (its response pipe failed, see above);"
                << " this thread's stream ends here and the thread runs on natively" << std::endl;
   if (response)
   {
      delete response;
      response = NULL;
   }
   if (output)
   {
      delete output;
      output = NULL;
   }
}

void Sift::Writer::frontEndStop()
{
  // Front end has been shut-down, shut-down ourselves as well...
  // exit() from here skips the front end's own exit handlers, so no other
  // thread's stream is closed with its End record: say so, or the simulator's
  // "stream ended without its End record" for those threads has no cause.
  std::cerr << "[SIFT:" << m_id << "] the simulator asked the front end to stop (Shutdown);"
            << " exiting the recording process now" << std::endl;
  exit(0);
}
