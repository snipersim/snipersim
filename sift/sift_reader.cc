#include "sift_reader.h"
#include <errno.h>
#include <string.h>
#include "sift_format.h"
#include "sift_utils.h"
#include "zfstream.h"

#include <array>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(PINPLAY) && !defined(PIN_CRT)
#include "champsim/trace_instruction.h"
#include "champsim/inf_stream.h"
#endif

// Enable (>0) to print out everything we read
#define VERBOSE 0
#define VERBOSE_HEX 0
#define VERBOSE_ICACHE 0

// #define DEBUG_SIFT_READER

#if !defined(PINPLAY) && !defined(PIN_CRT)
namespace
{
   // Wrap a plain std::ifstream as a ChampSimStream
   struct IfstreamChampSimStream : public Sift::Reader::ChampSimStream
   {
      std::ifstream *stream;
      explicit IfstreamChampSimStream(std::ifstream *s) : stream(s) {}
      size_t read_bytes(void *dst, size_t len) override
      {
         stream->read(reinterpret_cast<char*>(dst), len);
         return static_cast<size_t>(stream->gcount());
      }
   };

   // Wrap ChampSim's inflating stream as a ChampSimStream
   template<typename Tag>
   struct InflatingChampSimStream : public Sift::Reader::ChampSimStream
   {
      champsim::inf_istream<Tag> stream;
      explicit InflatingChampSimStream(const std::string &fname) : stream(fname) {}
      size_t read_bytes(void *dst, size_t len) override
      {
         stream.read(reinterpret_cast<char*>(dst), len);
         return static_cast<size_t>(stream.gcount());
      }
   };
}
#endif // !PINPLAY

namespace Sift
{

Reader::Reader(const char *filename, const char *response_filename, uint32_t id, TraceFormat format)
   : input(NULL)
   , response(NULL)
   , handleInstructionCountFunc(NULL)
   , handleInstructionCountArg(NULL)
   , handleCacheOnlyFunc(NULL)
   , handleCacheOnlyArg(NULL)
   , handleOutputFunc(NULL)
   , handleOutputArg(NULL)
   , handleSyscallFunc(NULL)
   , handleSyscallArg(NULL)
   , handleNewThreadFunc(NULL)
   , handleNewThreadArg(NULL)
   , handleForkFunc(NULL)
   , handleForkArg(NULL)
   , handleJoinFunc(NULL)
   , handleJoinArg(NULL)
   , handleMagicFunc(NULL)
   , handleMagicArg(NULL)
   , handleEmuFunc(NULL)
   , handleEmuArg(NULL)
   , handleRoutineChangeFunc(NULL)
   , handleRoutineAnnounceFunc(NULL)
   , handleRoutineArg(NULL)
   , filesize(0)
   , inputstream(NULL)
   , last_address(0)
   , icache()
   , scache()
   , vcache()
   , m_id(id)
   , m_trace_has_pa(false)
   , m_seen_end(false)
   , m_pending_spans_n(0)
   , m_pending_spans_dropped(0)
   , m_last_sinst(NULL)
   , m_code_may_change(false)
   , m_isa(0)
   , m_format(format)
   , m_champsim_stream()
{
   m_filename = strdup(filename);
   m_response_filename = strdup(response_filename);
}

Reader::~Reader()
{
   free(m_filename);
   free(m_response_filename);

   // Ownership: vifstream deletes the ifstream it was constructed from, and
   // izstream (the compressed case) deletes the vifstream, so deleting `input`
   // takes `inputstream` with it.  Deleting it again here was a double free
   // that crashed every reader of an uncompressed trace on the way out --
   // siftdump exited 139 after printing the whole trace correctly.
   //
   // The ChampSim path is the exception: IfstreamChampSimStream only borrows
   // the stream, so there `inputstream` is still ours to delete.
   if (input)
   {
      delete input;
      inputstream = NULL;
   }
   if (response)
      delete response;
   if (inputstream)
      delete inputstream;

   for (std::unordered_map<uint64_t, const uint8_t*>::iterator i = icache.begin(); i != icache.end(); ++i)
      delete [] (*i).second;

   for (std::unordered_map<uint64_t, const StaticInstruction*>::iterator i = scache.begin(); i != scache.end(); ++i)
      delete (*i).second;
}

void Reader::resetStream()
{
   if (inputstream)
   {
      inputstream->clear();
      inputstream->seekg(0);
   }
}

bool Reader::initStream()
{
#if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] InitStream Attempting Open" << std::endl;
#endif

#if !defined(PINPLAY) && !defined(PIN_CRT)
   std::string fname(m_filename);
   bool is_gz  = fname.size() >= 3 && fname.compare(fname.size() - 3, 3, ".gz") == 0;
   bool has_xz_extension = fname.size() >= 3 && fname.compare(fname.size() - 3, 3, ".xz") == 0;
   bool is_xz  = CHAMPSIM_HAS_LZMA && has_xz_extension;
   bool is_bz2 = fname.size() >= 4 && fname.compare(fname.size() - 4, 4, ".bz2") == 0;
   bool maybe_champsim_compressed = is_gz || is_xz || is_bz2;

   // Heuristic: compressed file with Champsim extension → likely ChampSim
   if (m_format == TraceFormat::Auto && maybe_champsim_compressed)
      m_format = TraceFormat::ChampSim;
#endif // !PINPLAY

   inputstream = new std::ifstream(m_filename, std::ios::in | std::ios::binary);
   if ((!inputstream->is_open()) || (!inputstream->good()))
   {
      std::cerr << "[SIFT:" << m_id << "] Cannot open " << m_filename << "\n";
      return false;
   }

   struct stat filestatus;
   if (stat(m_filename, &filestatus) == 0)
      filesize = filestatus.st_size;
   else
      filesize = 0;

#if !defined(PINPLAY) && !defined(PIN_CRT)
   if (has_xz_extension && !CHAMPSIM_HAS_LZMA)
   {
      std::cerr << "[SIFT:" << m_id << "] .xz traces require liblzma development headers at build time\n";
      return false;
   }

   // ChampSim-only path: no SIFT header
   if (m_format == TraceFormat::ChampSim)
   {
      std::cout << "[Frontend] Using ChampSim format for trace input\n";

      if (maybe_champsim_compressed)
      {
         if (is_gz)
            m_champsim_stream.reset(new InflatingChampSimStream<champsim::decomp_tags::gzip_tag_t<>>(fname));
#if CHAMPSIM_HAS_LZMA
         else if (is_xz)
            m_champsim_stream.reset(new InflatingChampSimStream<champsim::decomp_tags::lzma_tag_t<>>(fname));
#endif
         else if (is_bz2)
            m_champsim_stream.reset(new InflatingChampSimStream<champsim::decomp_tags::bzip2_tag_t>(fname));
      }
      else
      {
         m_champsim_stream.reset(new IfstreamChampSimStream(inputstream));
      }

      return true;
   }
#endif // !PINPLAY

   // Try SIFT header
   input = new vifstream(inputstream);

   Sift::Header hdr;
   input->read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
   if (hdr.magic != Sift::MagicNumber)
   {
#if !defined(PINPLAY) && !defined(PIN_CRT)
      // Fallback: if we were in Auto mode, try to interpret as ChampSim trace
      if (m_format == TraceFormat::Auto)
      {
         resetStream();
         m_format = TraceFormat::ChampSim;

         std::cout << "[Frontend] SIFT magic mismatch, falling back to ChampSim format\n";

         if (maybe_champsim_compressed)
         {
            if (is_gz)
               m_champsim_stream.reset(new InflatingChampSimStream<champsim::decomp_tags::gzip_tag_t<>>(fname));
#if CHAMPSIM_HAS_LZMA
            else if (is_xz)
               m_champsim_stream.reset(new InflatingChampSimStream<champsim::decomp_tags::lzma_tag_t<>>(fname));
#endif
            else if (is_bz2)
               m_champsim_stream.reset(new InflatingChampSimStream<champsim::decomp_tags::bzip2_tag_t>(fname));
         }
         else
         {
            m_champsim_stream.reset(new IfstreamChampSimStream(inputstream));
         }

         return true;
      }
      else
#endif // !PINPLAY
      {
         std::cerr << "[SIFT:" << m_id << "] Invalid magic number\n";
         return false;
      }
   }

   if (hdr.size != 0)
   {
      std::cerr << "[SIFT:" << m_id << "] Invalid header size\n";
   }

   m_format = TraceFormat::Sift;

#if SIFT_USE_ZLIB
   if (hdr.options & CompressionZlib)
   {
      input = new izstream(input);
      hdr.options &= ~CompressionZlib;
   }
#else
   if (hdr.options & CompressionZlib)
   {
      std::cerr << "[SIFT:" << m_id << "] Error: Compression requested, but disabled at compile time.\n";
   }
#endif

   // The ISA every instruction is decoded in until a RecOtherISAChange says
   // otherwise.  This used to be left at 0 -- DL_ISA_IA32 -- for every trace,
   // which was harmless only because the decoder ignored the value it was
   // given.  Now that it honours it, a 64-bit trace has to start out saying so.
   if (hdr.options & ArchIA32)
   {
      m_isa = 0;   // DL_ISA_IA32
      hdr.options &= ~ArchIA32;
   }
   else
   {
      m_isa = 1;   // DL_ISA_X86_64
   }

   if (hdr.options & PhysicalAddress)
   {
      m_trace_has_pa = true;
      hdr.options &= ~PhysicalAddress;
   }
   if (hdr.options & CodePhysical)
   {
      m_code_physical = true;
      hdr.options &= ~CodePhysical;
   }

   // Remember it rather than only clearing it: this is the trace telling us
   // the program rewrites its own code.
   m_code_may_change = (hdr.options & IcacheVariable) != 0;
   hdr.options &= ~IcacheVariable;

   // Make sure there are no unrecognized options
   if (hdr.options != 0)
   {
      return false;
   }

#if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] InitStream Connection Open" << std::endl;
#endif

   return true;
}

bool Reader::initResponse()
{
   if (!response)
   {
      if (strcmp(m_response_filename, "") == 0)
      {
         std::cerr << "[SIFT:" << m_id << "] Response filename not set\n";
         return false;
      }
      response = new vofstream(m_response_filename, std::ios::out);
   }

   if ((!response->is_open()) || (response->fail()))
   {
      std::cerr << "[SIFT:" << m_id << "] Cannot open " << m_response_filename << ": " << strerror(errno) << "\n";
      return false;
   }

   return true;
}

bool Reader::Read(Instruction &inst)
{
   // Initialise stream once (for either SIFT or ChampSim)
   if (input == NULL
#if !defined(PINPLAY) && !defined(PIN_CRT)
       && !m_champsim_stream
#endif // !PINPLAY
      )
   {
      if (!initStream())
      {
         std::cerr << "[SIFT:" << m_id << "] Error: initStream failed\n";
         return false;
      }
   }

   // Clear ChampSim-specific fields every time
   inst.is_champsim = false;
   inst.num_src_addresses = 0;
   inst.num_dest_addresses = 0;
   memset(inst.src_addresses, 0, sizeof(inst.src_addresses));
   memset(inst.dest_addresses, 0, sizeof(inst.dest_addresses));
   inst.num_src_registers = 0;
   inst.num_dest_registers = 0;
   memset(inst.src_registers, 0, sizeof(inst.src_registers));
   memset(inst.dest_registers, 0, sizeof(inst.dest_registers));

#if !defined(PINPLAY) && !defined(PIN_CRT)
   // ChampSim path
   if (m_format == TraceFormat::ChampSim)
   {
      return readChampSim(inst);
   }
#endif // !PINPLAY

   // Original SIFT path
   while(!m_seen_end)
   {
      Record rec;
      errno = 0;        // so that a failure below reports this read, not an earlier call
      uint8_t byte = input->peek();
      if (input->fail())
      {
         if (errno)
            std::cerr << "[SIFT:" << m_id << "] Error: " << strerror(errno) << "\n";
         else
            std::cerr << "[SIFT:" << m_id << "] stream ended without its End record: the recording"
                      << " process exited or crashed without closing it, or dropped this thread's"
                      << " channel (look for a [SIFT] or recorder message above)\n";
         return false;
      }

      if (byte == 0)
      {
         // "Other" record
         input->read(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
         switch(rec.Other.type)
         {
            case RecOtherEnd:
               assert(rec.Other.size == 0);
               m_seen_end = true;
               // Disable EndResponse as it causes lockups with sift_recorder
               // sendSimpleResponse(RecOtherEndResponse);
               return false;

            case RecOtherIcache:
            {
               assert(rec.Other.size == sizeof(uint64_t) + ICACHE_SIZE);
               uint64_t address;
               input->read(reinterpret_cast<char*>(&address), sizeof(uint64_t));
               /* address is the page's virtual address; it is held under its
                  key -- itself, or its physical page, whose mapping the
                  writer sent first */
               const uint64_t key = codeKey(address);
               auto known = icache.find(key);
               if (known == icache.end())
               {
                  uint8_t *bytes = new uint8_t[ICACHE_SIZE];
                  input->read(reinterpret_cast<char*>(bytes), ICACHE_SIZE);
                  icache[key] = bytes;
               }
               else
               {
                  // The page again: its code changed after it was first sent
                  // (CodeTracker in the writer).  Take the new bytes, and drop
                  // everything decoded from the old ones -- every instruction
                  // starting in the page, and any from the previous page that
                  // runs into it.
                  input->read(reinterpret_cast<char*>(const_cast<uint8_t*>(known->second)), ICACHE_SIZE);
                  // (15: the longest an instruction from the previous page can
                  // reach in; that page's key is not this one's minus a page
                  // when code is keyed physically)
                  const uint64_t prev_end = address >= ICACHE_SIZE ? codeKey(address - ICACHE_SIZE) + ICACHE_SIZE : 0;
                  for (uint64_t a = prev_end >= 15 ? prev_end - 15 : 0; a < prev_end; ++a)
                  {
                     auto s = scache.find(a);
                     if (s != scache.end() && s->second->key + s->second->size > prev_end)
                     {
                        const_cast<StaticInstruction*>(s->second)->dead = true;
                        scache.erase(s);
                     }
                  }
                  for (uint64_t a = key; a < key + ICACHE_SIZE; ++a)
                  {
                     auto s = scache.find(a);
                     if (s != scache.end())
                     {
                        const_cast<StaticInstruction*>(s->second)->dead = true;
                        scache.erase(s);
                     }
                  }
                  if (m_last_sinst && m_last_sinst->dead)
                     m_last_sinst = NULL;
               }
               break;
            }

            case RecOtherIcacheVariable:
            {
#if VERBOSE_ICACHE
               std::cerr << __FUNCTION__ << ": rec=" << std::endl;
               hexdump(&rec, sizeof(rec.Other));
#endif
               uint64_t address;
               size_t size = rec.Other.size - sizeof(uint64_t);
               input->read(reinterpret_cast<char*>(&address), sizeof(uint64_t));

               // New bytes for this address: anything already decoded from the
               // old ones describes an instruction that no longer exists.
               // Dropped rather than rewritten -- the old entry may still be
               // referenced as some other instruction's "next", and that
               // shortcut is disabled for these traces for exactly this reason.
               {
                  auto s = scache.find(address);
                  if (s != scache.end())
                  {
                     const_cast<StaticInstruction*>(s->second)->dead = true;
                     scache.erase(s);
                  }
               }
               m_last_sinst = NULL;

               size_t size_left = size;
               while (size_left > 0)
               {
                  uint64_t base_addr = address & ICACHE_PAGE_MASK;
                  if (icache.count(base_addr) == 0)
                     icache[base_addr] = new uint8_t[ICACHE_SIZE];
                  uint64_t offset = address & ICACHE_OFFSET_MASK;
                  size_t read_amount = std::min(size_left, size_t(ICACHE_SIZE - offset));
                  input->read(const_cast<char*>(reinterpret_cast<const char*>(&(icache[base_addr][offset]))), read_amount);

#if VERBOSE_ICACHE
                  std::cerr << __FUNCTION__ << ": Wrote " << read_amount << " bytes to 0x" << std::hex << (void*)&(icache[base_addr][offset]) << std::dec << std::endl;
                  hexdump(&(icache[base_addr][offset]), read_amount);
#endif

                  size_left -= read_amount;
                  address = base_addr + ICACHE_SIZE;
               }
               break;
            }

            case RecOtherLogical2Physical:
            {
               assert(rec.Other.size == 2 * sizeof(uint64_t));
               uint64_t vp, pp;
               input->read(reinterpret_cast<char*>(&vp), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&pp), sizeof(uint64_t));
               /* Frame ~0 (no such page) withdraws the mapping: the page now
                  maps a device, or nothing, and its accesses go by their
                  virtual address again (va2pa reports no mapping). */
               if (pp == ~0ULL)
                  vcache.erase(vp);
               else
                  vcache[vp] = pp;
               break;
            }

            case RecOtherInstructionCount:
            {
#if VERBOSE > 0
               std::cerr << "[DEBUG:" << m_id << "] Read InstructionCount" << std::endl;
#endif
               assert(rec.Other.size == sizeof(uint32_t));
               uint32_t icount;
               input->read(reinterpret_cast<char*>(&icount), sizeof(icount));
               Mode mode = ModeUnknown;
               if (handleInstructionCountFunc)
                  mode = handleInstructionCountFunc(handleInstructionCountArg, icount);
               sendSimpleResponse(RecOtherSyncResponse, &mode, sizeof(Mode));
               break;
            }

            case RecOtherCacheOnly:
            {
#if VERBOSE > 0
               std::cerr << "[DEBUG:" << m_id << "] Read CacheOnly" << std::endl;
#endif
               assert(rec.Other.size == sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint64_t));
               uint8_t icount, type;
               uint64_t eip, address;
               input->read(reinterpret_cast<char*>(&icount), sizeof(uint8_t));
               input->read(reinterpret_cast<char*>(&type), sizeof(uint8_t));
               input->read(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&address), sizeof(uint64_t));
               if (handleCacheOnlyFunc)
                  handleCacheOnlyFunc(handleCacheOnlyArg, icount, (Sift::CacheOnlyType)type, eip, address);
               break;
            }

            case RecOtherOutput:
            {
#if VERBOSE > 0
               std::cerr << "[DEBUG:" << m_id << "] Read Output" << std::endl;
#endif
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

            case RecOtherSyscallRequest:
            {
#if VERBOSE > 0
               std::cerr << "[DEBUG:" << m_id << "] Read SyscallRequest" << std::endl;
#endif
               assert(rec.Other.size > sizeof(uint16_t));
               uint16_t syscall_number;
               uint32_t size = rec.Other.size - sizeof(uint16_t);
               uint8_t *bytes = new uint8_t[size];
               input->read(reinterpret_cast<char*>(&syscall_number), sizeof(uint16_t));
               input->read(reinterpret_cast<char*>(bytes), size);
#if VERBOSE_HEX > 0
               hexdump((char*)&rec, sizeof(rec.Other));
               hexdump((char*)&syscall_number, sizeof(syscall_number));
               hexdump((char*)bytes, size);
#endif
#if VERBOSE > 1
               for (unsigned i = 0 ; i < (size/8) ; i++)
               {
                  std::cerr << __FUNCTION__ << ": syscall args[" << i << "] = " << ((uint64_t*)bytes)[i] << std::endl;
               }
#endif

               assert(handleSyscallFunc);
               if (handleSyscallFunc)
               {
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleSyscall" << std::endl;
#endif
                  uint64_t ret = handleSyscallFunc(handleSyscallArg, syscall_number, bytes, size);
                  sendSyscallResponse(ret);
               }
               delete [] bytes;
               break;
            }

            case RecOtherNewThread:
            {
               assert(rec.Other.size == 0);
               assert(handleNewThreadFunc);
               if (handleNewThreadFunc)
               {
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleNewThread" << std::endl;
#endif
                  int32_t ret = handleNewThreadFunc(handleNewThreadArg);
                  sendSimpleResponse(RecOtherNewThreadResponse, &ret, sizeof(ret));
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleNewThread Done" << std::endl;
#endif
               }
               break;
            }

            case RecOtherJoin:
            {
               int32_t thread;
               assert(rec.Other.size == sizeof(thread));
               input->read(reinterpret_cast<char*>(&thread), sizeof(thread));
               assert(handleJoinFunc);
               if (handleJoinFunc)
               {
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleJoin" << std::endl;
#endif
                  int32_t ret = handleJoinFunc(handleJoinArg, thread);
                  sendSimpleResponse(RecOtherJoinResponse, &ret, sizeof(ret));
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleJoin Done" << std::endl;
#endif
               }
               break;
            }

            case RecOtherSync:
            {
               assert(rec.Other.size == 0);
               Mode mode = ModeUnknown;
               if (handleInstructionCountFunc)
                  mode = handleInstructionCountFunc(handleInstructionCountArg, 0);
               sendSimpleResponse(RecOtherSyncResponse, &mode, sizeof(Mode));
               break;
            }

            case RecOtherFork:
            {
               assert(rec.Other.size == 0);
               assert(handleForkFunc);
               if(handleForkFunc)
               {
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleFork" << std::endl;
#endif
                  int32_t ret = handleForkFunc(handleForkArg);
                  sendSimpleResponse(RecOtherForkResponse, &ret, sizeof(ret));
#if VERBOSE > 0
                  std::cerr << "[DEBUG:" << m_id << "] HandleFork Done" << std::endl;
#endif
               }
               break;
            }

            case RecOtherMagicInstruction:
            {
               assert(rec.Other.size == 3 * sizeof(uint64_t));
               uint64_t a, b, c;
               input->read(reinterpret_cast<char*>(&a), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&b), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&c), sizeof(uint64_t));
               uint64_t result;
               /* Whether to answer is the handler's to say, apart from the
                  answer itself.  It used to be read off the answer -- 42 meant
                  "no response" -- so any command whose honest answer was 42
                  (a user command, SimGetProcId() on core 42, a 42 MHz clock)
                  was never answered and its program hung. */
               bool respond = true;
               if (handleMagicFunc)
                  result = handleMagicFunc(handleMagicArg, a, b, c, &respond);
               else
                  result = a; // Do not modify GAX register

               if (!respond)
                  return true;     // another stream runs next; see HandleMagicFunc

               sendSimpleResponse(RecOtherMagicInstructionResponse, &result, sizeof(result));
               break;
            }

            case RecOtherMagicInstructionNoResponse:
            {
               assert(rec.Other.size == 3 * sizeof(uint64_t));
               uint64_t a, b, c;
               input->read(reinterpret_cast<char*>(&a), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&b), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&c), sizeof(uint64_t));
               if (handleMagicFunc)
               {
                  bool respond = true;     // nothing to answer on, either way
                  handleMagicFunc(handleMagicArg, a, b, c, &respond);
               }
               break;
            }

            case RecOtherEmu:
            {
               assert(rec.Other.size <= sizeof(uint16_t) + sizeof(EmuRequest));
               uint16_t type; EmuRequest req;
               input->read(reinterpret_cast<char*>(&type), sizeof(uint16_t));
               input->read(reinterpret_cast<char*>(&req), rec.Other.size - sizeof(uint16_t));
               bool result = false; EmuReply res = {};
               if (handleEmuFunc)
               {
                  result = handleEmuFunc(handleEmuArg, EmuType(type), req, res);
               }
               sendEmuResponse(result, res);
               break;
            }

            case RecOtherRoutineChange:
            {
               assert(rec.Other.size == sizeof(uint8_t) + 3 * sizeof(uint64_t));
               uint8_t event;
               uint64_t eip, esp, callEip;
               input->read(reinterpret_cast<char*>(&event), sizeof(uint8_t));
               input->read(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&esp), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&callEip), sizeof(uint64_t));
               if (handleRoutineChangeFunc)
                  handleRoutineChangeFunc(handleRoutineArg, Sift::RoutineOpType(event), eip, esp, callEip);
               break;
            }

            case RecOtherRoutineAnnounce:
            {
               uint64_t eip, offset;
               uint16_t len_name, len_imgname, len_filename;
               char *name, *imgname, *filename;
               uint32_t line, column;
               input->read(reinterpret_cast<char*>(&eip), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&len_name), sizeof(uint16_t));
               name = (char*)malloc(len_name);
               input->read(name, len_name);
               input->read(reinterpret_cast<char*>(&len_imgname), sizeof(uint16_t));
               imgname = (char*)malloc(len_imgname);
               input->read(imgname, len_imgname);
               input->read(reinterpret_cast<char*>(&offset), sizeof(uint64_t));
               input->read(reinterpret_cast<char*>(&line), sizeof(uint32_t));
               input->read(reinterpret_cast<char*>(&column), sizeof(uint32_t));
               input->read(reinterpret_cast<char*>(&len_filename), sizeof(uint16_t));
               filename = (char*)malloc(len_filename);
               input->read(filename, len_filename);
               if (handleRoutineAnnounceFunc)
                  handleRoutineAnnounceFunc(handleRoutineArg, eip, name, imgname, offset, line, column, filename);
               free(name);
               free(imgname);
               free(filename);
               break;
            }

            case RecOtherISAChange:
            {
               assert(rec.Other.size == sizeof(uint32_t));
               uint32_t new_isa;
               input->read(reinterpret_cast<char*>(&new_isa), sizeof(new_isa));
               m_isa = new_isa;
               break;
            }

            case RecOtherMemorySpans:
            {
               uint8_t n;
               uint32_t dropped = 0;
               input->read(reinterpret_cast<char*>(&n), sizeof(n));
               assert(n <= MAX_MEMORY_SPANS);
               const size_t base = sizeof(uint8_t) + n * sizeof(MemorySpan);
               assert(rec.Other.size == base || rec.Other.size == base + sizeof(dropped));
               input->read(reinterpret_cast<char*>(m_pending_spans), n * sizeof(MemorySpan));
               if (rec.Other.size == base + sizeof(dropped))
                  input->read(reinterpret_cast<char*>(&dropped), sizeof(dropped));
               m_pending_spans_n = n;
               m_pending_spans_dropped = dropped;
               break;
            }
            default:
            {
               // Skip unknown Other record
               uint8_t *bytes = new uint8_t[rec.Other.size];
               input->read(reinterpret_cast<char*>(bytes), rec.Other.size);
               delete [] bytes;
               break;
            }
         }
         continue;
      }

      uint8_t size;
      uint64_t addr;

      if ((byte & 0xf) != 0)
      {
         // Instruction (small encoding)
         input->read(reinterpret_cast<char*>(&rec), sizeof(rec.Instruction));

#if VERBOSE_HEX > 2
         hexdump(&rec, sizeof(rec.Instruction));
#endif

         size = rec.Instruction.size;
         addr = last_address;
         inst.num_addresses = rec.Instruction.num_addresses;
         inst.is_branch = rec.Instruction.is_branch;
         inst.taken = rec.Instruction.taken;
         inst.is_predicate = false;
         inst.executed = true;
         inst.isa = m_isa;
      }
      else
      {
         // InstructionExt (full encoding)
         input->read(reinterpret_cast<char*>(&rec), sizeof(rec.InstructionExt));

#if VERBOSE_HEX > 2
         hexdump(&rec, sizeof(rec.InstructionExt));
#endif

         size = rec.InstructionExt.size;
         addr = rec.InstructionExt.addr;
         inst.num_addresses = rec.InstructionExt.num_addresses;
         inst.is_branch = rec.InstructionExt.is_branch;
         inst.taken = rec.InstructionExt.taken;
         inst.is_predicate = rec.InstructionExt.is_predicate;
         inst.executed = rec.InstructionExt.executed;
         inst.isa = m_isa;

         last_address = addr;
      }

      last_address += size;
      inst.num_spans = m_pending_spans_n;
      inst.spans_dropped_lines = m_pending_spans_dropped;
      if (m_pending_spans_n)
      {
         memcpy(inst.spans, m_pending_spans, m_pending_spans_n * sizeof(MemorySpan));
         m_pending_spans_n = 0;
      m_pending_spans_dropped = 0;
      }
      for (int i = 0; i < inst.num_addresses; ++i)
         input->read(reinterpret_cast<char*>(&inst.addresses[i]), sizeof(uint64_t));

      inst.sinst = getStaticInstruction(addr, size);

#if VERBOSE_HEX > 2
      hexdump(inst.sinst->data, inst.sinst->size);
#endif
#if VERBOSE > 2
      printf("%016lx (%d) A%u %c%c %c%c\n",
             inst.sinst->addr, inst.sinst->size, inst.num_addresses,
             inst.is_branch?'B':'.',
             inst.is_branch?(inst.taken?'T':'.'):'.',
             inst.is_predicate?'C':'.',
             inst.is_predicate?(inst.executed?'E':'n'):'.');
#endif
      return true;
   }

   // Should only reach here after RecOtherEnd
   return true;
}

#if !defined(PINPLAY) && !defined(PIN_CRT)
bool Reader::readChampSim(Instruction &inst)
{
   input_instr cs_inst{};
   size_t read_bytes = 0;

   if (m_champsim_stream)
   {
      read_bytes = m_champsim_stream->read_bytes(&cs_inst, sizeof(cs_inst));
   }
   else if (inputstream)
   {
      inputstream->read(reinterpret_cast<char*>(&cs_inst), sizeof(cs_inst));
      read_bytes = static_cast<size_t>(inputstream->gcount());
   }

   if (read_bytes != sizeof(cs_inst))
      return false;

   inst.is_champsim = true;
   inst.is_branch   = cs_inst.is_branch;
   inst.taken       = cs_inst.branch_taken;
   inst.is_predicate = false;
   inst.executed    = true;
   inst.isa         = m_isa;

   // Collect destination registers
   for (uint8_t idx = 0; idx < CHAMPSIM_MAX_DEST_REGISTERS; ++idx)
   {
      if (cs_inst.destination_registers[idx])
      {
         inst.dest_registers[inst.num_dest_registers++] = cs_inst.destination_registers[idx];
      }
   }

   // Collect source registers
   for (uint8_t idx = 0; idx < CHAMPSIM_MAX_SRC_REGISTERS; ++idx)
   {
      if (cs_inst.source_registers[idx])
      {
         inst.src_registers[inst.num_src_registers++] = cs_inst.source_registers[idx];
      }
   }

   // Collect destination memory addresses
   for (uint8_t idx = 0; idx < CHAMPSIM_MAX_DEST_ADDRESSES; ++idx)
   {
      if (cs_inst.destination_memory[idx])
      {
         inst.dest_addresses[inst.num_dest_addresses++] = cs_inst.destination_memory[idx];
      }
   }

   // Collect source memory addresses
   for (uint8_t idx = 0; idx < CHAMPSIM_MAX_SRC_ADDRESSES; ++idx)
   {
      if (cs_inst.source_memory[idx])
      {
         inst.src_addresses[inst.num_src_addresses++] = cs_inst.source_memory[idx];
      }
   }

   // Fill legacy addresses[] array with src first, then dest, but keep MAX_DYNAMIC_ADDRESSES bound
   inst.num_addresses = std::min<uint8_t>(inst.num_src_addresses + inst.num_dest_addresses, MAX_DYNAMIC_ADDRESSES);
   inst.num_spans = 0;
   uint8_t combined_idx = 0;

   for (uint8_t i = 0; i < inst.num_src_addresses && combined_idx < inst.num_addresses; ++i, ++combined_idx)
   {
      inst.addresses[combined_idx] = inst.src_addresses[i];
   }
   for (uint8_t i = 0; i < inst.num_dest_addresses && combined_idx < inst.num_addresses; ++i, ++combined_idx)
   {
      inst.addresses[combined_idx] = inst.dest_addresses[i];
   }

   inst.sinst = getChampSimStaticInstruction(cs_inst.ip);
   last_address = cs_inst.ip + inst.sinst->size;

   return true;
}
#endif // !PINPLAY

bool Reader::AccessMemory(MemoryLockType lock_signal, MemoryOpType mem_op, uint64_t d_addr, uint8_t *data_buffer, uint32_t data_size)
{
#ifdef DEBUG_SIFT_READER
   if (mem_op == MemWrite)
      std::cerr << "[DEBUG:" << m_id << "] MemoryRequest - Write - addr = " << d_addr  << std::endl;
   if (mem_op == MemRead)
      std::cerr << "[DEBUG:" << m_id << "] MemoryRequest - Read - addr = " << d_addr << std::endl;
#endif

#if !defined(PINPLAY) && !defined(PIN_CRT)
   // ChampSim traces are offline, there is no live front-end to answer memory requests.
   if (m_format == TraceFormat::ChampSim)
   {
      std::cerr << "[SIFT:" << m_id << "] AccessMemory not supported for ChampSim traces\n";
      return false;
   }
#endif // !PINPLAY

   if (input == NULL)
   {
      if (!initStream())
      {
         std::cerr << "[SIFT:" << m_id << "] Error: initStream failed\n";
         return false;
      }
   }

   if (!initResponse())
   {
      std::cerr << "[SIFT:" << m_id << "] Error: initResponse failed\n";
      return false;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherMemoryRequest;
   rec.Other.size = sizeof(d_addr) + sizeof(data_size) + sizeof(lock_signal) + sizeof(mem_op);
   if (mem_op == MemWrite)
   {
      rec.Other.size += data_size;
   }
   response->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   response->write(reinterpret_cast<char*>(&d_addr), sizeof(d_addr));
   response->write(reinterpret_cast<char*>(&data_size), sizeof(data_size));
   response->write(reinterpret_cast<char*>(&lock_signal), sizeof(lock_signal));
   response->write(reinterpret_cast<char*>(&mem_op), sizeof(mem_op));
   if (mem_op == MemWrite)
   {
      response->write(reinterpret_cast<char*>(data_buffer), data_size);
   }
   response->flush();

#if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Read MemoryResponse" << std::endl;
#endif
   uint64_t addr;
   MemoryOpType type;
   input->read(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
#if VERBOSE_HEX > 0
   hexdump((char*)&rec, sizeof(rec.Other));
#endif
   if (rec.Other.type != RecOtherMemoryResponse)
   {
      std::cerr << "[SIFT:" << m_id << "] Error: Invalid response. Expected RecOtherMemoryResponse\n";
      return false;
   }
   input->read(reinterpret_cast<char*>(&addr), sizeof(addr));
   input->read(reinterpret_cast<char*>(&type), sizeof(type));
#if VERBOSE_HEX > 0
   hexdump((char*)&addr, sizeof(addr));
   hexdump((char*)&type, sizeof(type));
#endif
   if (addr != d_addr)
   {
      std::cerr << "[SIFT:" << m_id << "] Error: Invalid response. Expected addresses to match\n";
      return false;
   }

   uint32_t payload_size = rec.Other.size - sizeof(addr) - sizeof(type);

   if (mem_op == MemRead)
   {
#if VERBOSE > 0
      std::cerr << "[DEBUG:" << m_id << "] Read MemoryResponse - Read Return Data" << std::endl;
#endif
      if (data_size != payload_size)
      {
         std::cerr << "[SIFT:" << m_id << "] Error: Invalid response. Expected payload size to match\n";
         return false;
      }
      input->read(reinterpret_cast<char*>(data_buffer), data_size);
#if VERBOSE_HEX > 0
      hexdump((char*)data_buffer, data_size);
#endif
   }
   else if (mem_op == MemWrite)
   {
      if (payload_size != 0)
      {
         std::cerr << "[SIFT:" << m_id << "] Error: Invalid response. Expected payload size of 0 for MemWrite\n";
         return false;
      }
   }
   else
   {
      std::cerr << "Sift::Reader::" << __FUNCTION__ << ": invalid return memory op type" << std::endl;
      return false;
   }

   return true;
}

const StaticInstruction* Reader::staticInfoInstruction(uint64_t addr, uint8_t size)
{
   StaticInstruction *sinst = new StaticInstruction();
   sinst->addr = addr;
   sinst->key = m_code_physical ? (codeKey(addr & ICACHE_PAGE_MASK) | (addr & ICACHE_OFFSET_MASK)) : addr;
   sinst->size = size;
   sinst->next = NULL;

   uint8_t * dst = sinst->data;
   uint64_t base_addr = addr & ICACHE_PAGE_MASK;
   while(size > 0)
   {
      uint32_t offset = (dst == sinst->data) ? addr & ICACHE_OFFSET_MASK : 0;
      uint32_t _size = std::min(uint32_t(size), ICACHE_SIZE - offset);
      const uint64_t key = codeKey(base_addr);
      assert(icache.count(key));
      memcpy(dst, icache[key] + offset, _size);
      dst += _size;
      size -= _size;
      base_addr += ICACHE_SIZE;
   }

   return sinst;
}

const StaticInstruction* Reader::getStaticInstruction(uint64_t addr, uint8_t size)
{
#if !defined(PINPLAY) && !defined(PIN_CRT)
   if (m_format == TraceFormat::ChampSim)
      return getChampSimStaticInstruction(addr);
#endif // !PINPLAY

   const StaticInstruction *sinst;
   const uint64_t key = m_code_physical ? (codeKey(addr & ICACHE_PAGE_MASK) | (addr & ICACHE_OFFSET_MASK)) : addr;

   // Lookup in a large unordered_map is quite expensive if we have to do this for every dynamic instruction
   // Therefore, keep a pointer to the probable next instruction in each (static) instruction
   if (!m_code_may_change && m_last_sinst && m_last_sinst->next && m_last_sinst->next->key == key
       && !m_last_sinst->next->dead)
   {
      sinst = m_last_sinst->next;
   }
   else if (scache.count(key) && scache[key]->size == size)
   {
      sinst = scache[key];
   }
   else
   {
      sinst = staticInfoInstruction(addr, size);
      scache[key] = sinst;
   }

   // Link, or re-link when the old successor's code was replaced
   if (m_last_sinst && (m_last_sinst->next == NULL || m_last_sinst->next->dead))
   {
      ((StaticInstruction*)m_last_sinst)->next = sinst;
   }
   m_last_sinst = sinst;

   return sinst;
}

#if !defined(PINPLAY) && !defined(PIN_CRT)
const StaticInstruction* Reader::getChampSimStaticInstruction(uint64_t addr)
{
   const StaticInstruction *sinst;

   if (m_last_sinst && m_last_sinst->next && m_last_sinst->next->addr == addr)
   {
      sinst = m_last_sinst->next;
   }
   else if (scache.count(addr))
   {
      sinst = scache[addr];
   }
   else
   {
      StaticInstruction *new_sinst = new StaticInstruction();
      new_sinst->key = addr;
      new_sinst->addr = addr;
      new_sinst->size = CHAMPSIM_DEFAULT_INST_SIZE;
      memset(new_sinst->data, 0, sizeof(new_sinst->data));
      new_sinst->next = NULL;
      scache[addr] = new_sinst;
      sinst = new_sinst;
   }

   if (m_last_sinst && m_last_sinst->next == NULL)
   {
      ((StaticInstruction*)m_last_sinst)->next = sinst;
   }
   m_last_sinst = sinst;

   return sinst;
}
#endif // !PINPLAY

void Reader::sendSyscallResponse(uint64_t return_code)
{
#if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write SyscallResponse" << std::endl;
#endif

   if (!initResponse())
   {
      std::cerr << "[SIFT:" << m_id << "] Error: initResponse failed\n";
      return;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherSyscallResponse;
   rec.Other.size = sizeof(return_code);
   response->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   response->write(reinterpret_cast<char*>(&return_code), sizeof(return_code));
   response->flush();
}

void Reader::sendEmuResponse(bool handled, EmuReply res)
{
#if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write sendEmuResponse" << std::endl;
#endif

   if (!initResponse())
   {
      std::cerr << "[SIFT:" << m_id << "] Error: initResponse failed\n";
      return;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = RecOtherEmuResponse;
   rec.Other.size = sizeof(uint8_t) + sizeof(EmuReply);
   uint8_t result = handled;
   response->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   response->write(reinterpret_cast<char*>(&result), sizeof(uint8_t));
   response->write(reinterpret_cast<char*>(&res), sizeof(EmuReply));
   response->flush();
}

void Reader::sendSimpleResponse(RecOtherType type, void *data, uint32_t size)
{
#if VERBOSE > 0
   std::cerr << "[DEBUG:" << m_id << "] Write SimpleResponse type=" << type << std::endl;
#endif

   if (!initResponse())
   {
      std::cerr << "[SIFT:" << m_id << "] Error: initResponse failed\n";
      return;
   }

   Record rec;
   rec.Other.zero = 0;
   rec.Other.type = type;
   rec.Other.size = size;
   response->write(reinterpret_cast<char*>(&rec), sizeof(rec.Other));
   if (size > 0)
      response->write(reinterpret_cast<char*>(data), size);
   response->flush();
}

uint64_t Reader::getPosition()
{
   if (inputstream)
      return static_cast<uint64_t>(inputstream->tellg());
   else
      return 0;
}

uint64_t Reader::getLength()
{
   return filesize;
}

/* The key a code page is held under: its virtual address, or in a
   CodePhysical trace its physical page, from the latest mapping the writer
   sent (Writer::codeKey is the other half) */
uint64_t Reader::codeKey(uint64_t va_page) const
{
   if (!m_code_physical)
      return va_page;
   auto it = vcache.find(va_page / PAGE_SIZE_SIFT);
   return it == vcache.end() ? va_page : ((uint64_t)it->second * PAGE_SIZE_SIFT) | CODE_KEY_PHYSICAL;
}

uint64_t Reader::va2pa(uint64_t va)
{
   if (m_trace_has_pa)
   {
      intptr_t vp = va / PAGE_SIZE_SIFT;
      intptr_t vo = va & (PAGE_SIZE_SIFT-1);

      if (vcache.count(vp) == 0)
      {
         return 0;
      }
      else
      {
         intptr_t pp = vcache[vp];
         return (pp * PAGE_SIZE_SIFT) | vo;
      }
   }
   else
   {
      return va;
   }
}

void Reader::frontEndStop()
{
   /* Once: the stopped thread itself and the end of the run (the reader pool)
      can both ask, and the second record would go to a front end that has
      closed its end */
   if (response && !m_shutdown_sent.exchange(true))
      sendSimpleResponse(RecOtherType::RecOtherShutdown, NULL, 0);
}

} // namespace Sift
