#ifndef __SIFT_READER_H
#define __SIFT_READER_H

#include "sift.h"
#include "sift_format.h"

extern "C" {
#include "xed-interface.h"
}

#include <unordered_map>
#include <fstream>
#include <cassert>

class vistream;

namespace Sift
{
   // Static information
   class StaticInstruction
   {
      public:
         uint64_t addr;
         uint8_t size;
         uint8_t data[16];
         const xed_decoded_inst_t xed_inst;
         const StaticInstruction *next;
   };

   // Dynamic information
   typedef struct
   {
      const StaticInstruction *sinst;
      uint8_t num_addresses;
      uint64_t addresses[MAX_DYNAMIC_ADDRESSES];
      bool is_branch;
      bool taken;
      bool is_predicate;
      bool executed;
   } Instruction;

   class Reader
   {
      typedef void (*HandleOutputFunc)(void* arg, uint8_t fd, const uint8_t *data, uint32_t size);
      typedef uint64_t (*HandleSyscallFunc)(void* arg, uint16_t syscall_number, const uint8_t *data, uint32_t size);
      typedef int32_t (*HandleNewThreadFunc)(void* arg);
      typedef int32_t (*HandleJoinFunc)(void* arg, int32_t thread);
      typedef uint64_t (*HandleMagicFunc)(void* arg, uint64_t a, uint64_t b, uint64_t c);
      typedef bool (*HandleEmuFunc)(void* arg, Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res);
      typedef void (*HandleRoutineChange)(void* arg, uint64_t eip, uint64_t esp, Sift::RoutineOpType event);
      typedef void (*HandleRoutineAnnounce)(void* arg, uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename);

      private:
         vistream *input;
         std::ofstream *response;
         HandleOutputFunc handleOutputFunc;
         void *handleOutputArg;
         HandleSyscallFunc handleSyscallFunc;
         void *handleSyscallArg;
         HandleNewThreadFunc handleNewThreadFunc;
         void *handleNewThreadArg;
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

         static bool xed_initialized;
         xed_state_t m_xed_state_init;

         uint64_t last_address;
         std::unordered_map<uint64_t, const uint8_t*> icache;
         std::unordered_map<uint64_t, const StaticInstruction*> scache;
         std::unordered_map<uint64_t, uint64_t> vcache;

         uint32_t m_id;

         bool m_trace_has_pa;
         bool m_seen_end;
         const StaticInstruction *m_last_sinst;

         const Sift::StaticInstruction* decodeInstruction(uint64_t addr, uint8_t size);
         const Sift::StaticInstruction* getStaticInstruction(uint64_t addr, uint8_t size);
         void sendSyscallResponse(uint64_t return_code);
         void sendEmuResponse(bool handled, EmuReply res);
         void sendSimpleResponse(RecOtherType type, void *data = NULL, uint32_t size = 0);

      public:
         Reader(const char *filename, const char *response_filename = "", uint32_t id = 0);
         ~Reader();
         void initStream();
         bool Read(Instruction&);
         void AccessMemory(MemoryLockType lock_signal, MemoryOpType mem_op, uint64_t d_addr, uint8_t *data_buffer, uint32_t data_size);

         void setHandleOutputFunc(HandleOutputFunc func, void* arg = NULL) { handleOutputFunc = func; handleOutputArg = arg; }
         void setHandleSyscallFunc(HandleSyscallFunc func, void* arg = NULL) { assert(func); handleSyscallFunc = func; handleSyscallArg = arg; }
         void setHandleNewThreadFunc(HandleNewThreadFunc func, void* arg = NULL) { assert(func); handleNewThreadFunc = func; handleNewThreadArg = arg; }
         void setHandleJoinFunc(HandleJoinFunc func, void* arg = NULL) { assert(func); handleJoinFunc = func; handleJoinArg = arg; }
         void setHandleMagicFunc(HandleMagicFunc func, void* arg = NULL) { assert(func); handleMagicFunc = func; handleMagicArg = arg; }
         void setHandleEmuFunc(HandleEmuFunc func, void* arg = NULL) { assert(func); handleEmuFunc = func; handleEmuArg = arg; }
         void setHandleRoutineFunc(HandleRoutineChange funcChange, HandleRoutineAnnounce funcAnnounce, void* arg = NULL) { assert(funcChange); assert(funcAnnounce); handleRoutineChangeFunc = funcChange; handleRoutineAnnounceFunc = funcAnnounce; handleRoutineArg = arg; }

         uint64_t getPosition();
         uint64_t getLength();
         bool getTraceHasPhysicalAddresses() const { return m_trace_has_pa; }
         uint64_t va2pa(uint64_t va);
   };
};

#endif // __SIFT_READER_H
