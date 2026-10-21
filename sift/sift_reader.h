#ifndef __SIFT_READER_H
#define __SIFT_READER_H

#include "sift.h"
#include "sift_format.h"
#include "sift_utils.h"

#include <unordered_map>
#include <fstream>
#include <cassert>
#include <memory>
#include <atomic>

class vistream;
class vostream;

namespace Sift
{
   // Additional trace formats beyond the native SIFT encoding.
   enum class TraceFormat
   {
      Auto,
      Sift,
      ChampSim,
   };

   const uint32_t CHAMPSIM_MAX_SRC_ADDRESSES = 4;
   const uint32_t CHAMPSIM_MAX_DEST_ADDRESSES = 2;
   const uint32_t CHAMPSIM_MAX_SRC_REGISTERS = 4;
   const uint32_t CHAMPSIM_MAX_DEST_REGISTERS = 2;
   const uint32_t CHAMPSIM_DEFAULT_INST_SIZE = 4;
   const uint32_t CHAMPSIM_DEFAULT_MEM_SIZE = 4;
   // Static information
   class StaticInstruction
   {
      public:
         //StaticInstruction();  // using default ctor

         uint64_t addr;
         /* What identifies the code: addr, or in a CodePhysical trace the
            physical address (with CODE_KEY_PHYSICAL), so that the same
            virtual address in two processes is two instructions */
         uint64_t key = 0;
         uint8_t size;
         uint8_t data[16];
         const StaticInstruction *next;
         // The code this was decoded from has since been replaced: the object
         // is kept (others may still point to it as their "next") but is never
         // handed out again.
         bool dead = false;
   };

   // Dynamic information
   typedef struct
   {
      const StaticInstruction *sinst;
      uint8_t num_addresses;
      uint64_t addresses[MAX_DYNAMIC_ADDRESSES];
      uint8_t num_spans;
      MemorySpan spans[MAX_MEMORY_SPANS];
      uint32_t spans_dropped_lines;      /* lines touched beyond the recorded spans */
      bool is_branch;
      bool taken;
      bool is_predicate;
      bool executed;
      int isa;

      // ChampSim-specific metadata (only valid when read from a ChampSim trace)
      bool is_champsim;
      uint8_t num_src_addresses;
      uint8_t num_dest_addresses;
      uint64_t src_addresses[CHAMPSIM_MAX_SRC_ADDRESSES];
      uint64_t dest_addresses[CHAMPSIM_MAX_DEST_ADDRESSES];
      uint8_t num_src_registers;
      uint8_t num_dest_registers;
      uint8_t src_registers[CHAMPSIM_MAX_SRC_REGISTERS];
      uint8_t dest_registers[CHAMPSIM_MAX_DEST_REGISTERS];
   } Instruction;

   class Reader
   {
      typedef Mode (*HandleInstructionCountFunc)(void* arg, uint32_t icount);
      typedef void (*HandleCacheOnlyFunc)(void* arg, uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address);
      typedef void (*HandleOutputFunc)(void* arg, uint8_t fd, const uint8_t *data, uint32_t size);
      typedef uint64_t (*HandleSyscallFunc)(void* arg, uint16_t syscall_number, const uint8_t *data, uint32_t size);
      typedef int32_t (*HandleNewThreadFunc)(void* arg);
      typedef int32_t (*HandleJoinFunc)(void* arg, int32_t thread);
      /* A magic instruction: returns the answer, and clears *respond when the
         caller must not be answered (a MimicOS context switch: another stream
         runs next, and the caller is answered later, when it is switched
         back to -- sendResponseAfterContextSwitch). */
      typedef uint64_t (*HandleMagicFunc)(void* arg, uint64_t a, uint64_t b, uint64_t c, bool *respond);
      typedef bool (*HandleEmuFunc)(void* arg, Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res);
      typedef void (*HandleRoutineChange)(void* arg, Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip);
      typedef void (*HandleRoutineAnnounce)(void* arg, uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename);
      typedef int32_t (*HandleForkFunc)(void* arg);

      public:
      struct ChampSimStream {
         virtual ~ChampSimStream() {}
         virtual size_t read_bytes(void *dst, size_t len) = 0;
      };

      private:
         vistream *input;
         vostream *response;
         HandleInstructionCountFunc handleInstructionCountFunc;
         void *handleInstructionCountArg;
         HandleCacheOnlyFunc handleCacheOnlyFunc;
         void *handleCacheOnlyArg;
         HandleOutputFunc handleOutputFunc;
         void *handleOutputArg;
         HandleSyscallFunc handleSyscallFunc;
         void *handleSyscallArg;
         HandleNewThreadFunc handleNewThreadFunc;
         void *handleNewThreadArg;
         HandleForkFunc handleForkFunc;
         void *handleForkArg;
         HandleJoinFunc handleJoinFunc;
         void *handleJoinArg;
         HandleMagicFunc handleMagicFunc;
         void *handleMagicArg;
         HandleEmuFunc handleEmuFunc;
         void *handleEmuArg;
         HandleRoutineChange handleRoutineChangeFunc;
         HandleRoutineAnnounce handleRoutineAnnounceFunc;
         void *handleRoutineArg;
         uint64_t filesize;
         std::ifstream *inputstream;

         char *m_filename;
         char *m_response_filename;

         uint64_t last_address;
         std::unordered_map<uint64_t, const uint8_t*> icache;
         std::unordered_map<uint64_t, const StaticInstruction*> scache;
         std::unordered_map<uint64_t, uint64_t> vcache;

         uint32_t m_id;

         bool m_trace_has_pa;
         bool m_code_physical = false;   // code pages are keyed by physical page (CodePhysical)
         uint64_t codeKey(uint64_t va_page) const;
         bool m_seen_end;
         uint8_t m_pending_spans_n;
         MemorySpan m_pending_spans[MAX_MEMORY_SPANS];
         uint32_t m_pending_spans_dropped;
         const StaticInstruction *m_last_sinst;
         // Set when the trace declares IcacheVariable: the writer is sending
         // each instruction's bytes because they can change at the same
         // address.  Static instructions then cannot be cached by address
         // alone, and the pointer-to-next shortcut cannot be trusted either.
         bool m_code_may_change;

         int m_isa;
         TraceFormat m_format;
         std::unique_ptr<ChampSimStream> m_champsim_stream;

         bool initResponse();
         const Sift::StaticInstruction* staticInfoInstruction(uint64_t addr, uint8_t size);
         const Sift::StaticInstruction* getStaticInstruction(uint64_t addr, uint8_t size);
         const Sift::StaticInstruction* getChampSimStaticInstruction(uint64_t addr);
         void sendSyscallResponse(uint64_t return_code);
         void sendEmuResponse(bool handled, EmuReply res);
         void sendSimpleResponse(RecOtherType type, void *data = NULL, uint32_t size = 0);
         bool readChampSim(Instruction &inst);
         void resetStream();

      public:
         Reader(const char *filename, const char *response_filename = "", uint32_t id = 0, TraceFormat format = TraceFormat::Auto);
         ~Reader();
         bool initStream();
         bool Read(Instruction&);
         bool AccessMemory(MemoryLockType lock_signal, MemoryOpType mem_op, uint64_t d_addr, uint8_t *data_buffer, uint32_t data_size);

         void setHandleInstructionCountFunc(HandleInstructionCountFunc func, void* arg = NULL) { handleInstructionCountFunc = func; handleInstructionCountArg = arg; }
         void setHandleCacheOnlyFunc(HandleCacheOnlyFunc func, void* arg = NULL) { handleCacheOnlyFunc = func; handleCacheOnlyArg = arg; }
         void setHandleOutputFunc(HandleOutputFunc func, void* arg = NULL) { handleOutputFunc = func; handleOutputArg = arg; }
         void setHandleSyscallFunc(HandleSyscallFunc func, void* arg = NULL) { assert(func); handleSyscallFunc = func; handleSyscallArg = arg; }
         void setHandleNewThreadFunc(HandleNewThreadFunc func, void* arg = NULL) { assert(func); handleNewThreadFunc = func; handleNewThreadArg = arg; }
         void setHandleJoinFunc(HandleJoinFunc func, void* arg = NULL) { assert(func); handleJoinFunc = func; handleJoinArg = arg; }
         void setHandleMagicFunc(HandleMagicFunc func, void* arg = NULL) { assert(func); handleMagicFunc = func; handleMagicArg = arg; }
         void setHandleEmuFunc(HandleEmuFunc func, void* arg = NULL) { assert(func); handleEmuFunc = func; handleEmuArg = arg; }
         void setHandleRoutineFunc(HandleRoutineChange funcChange, HandleRoutineAnnounce funcAnnounce, void* arg = NULL) { assert(funcChange); assert(funcAnnounce); handleRoutineChangeFunc = funcChange; handleRoutineAnnounceFunc = funcAnnounce; handleRoutineArg = arg; }
         void setHandleForkFunc(HandleForkFunc func, void* arg = NULL) { assert(func); handleForkFunc = func; handleForkArg = arg;}

         uint64_t getPosition();
         uint64_t getLength();
         bool getTraceHasPhysicalAddresses() const { return m_trace_has_pa; }
         bool isChampSimTrace() const { return m_format == TraceFormat::ChampSim; }
         TraceFormat getFormat() const { return m_format; }
         uint64_t va2pa(uint64_t va);
         void sendResponseAfterContextSwitch()
         {
            uint64_t ack = 0;
            sendSimpleResponse(RecOtherMagicInstructionResponse, &ack, sizeof(ack)); // Send a pointer as a response
         }

         char* getFilename() const { 
            return m_filename;
         } 

         char* getResponseFilename() const { 
            return m_response_filename;
         } 

         int getId() const {
            return m_id;
         }


	 void frontEndStop();
      private:
         std::atomic<bool> m_shutdown_sent{false};   // frontEndStop() has sent its answer
   };
};

#endif // __SIFT_READER_H
