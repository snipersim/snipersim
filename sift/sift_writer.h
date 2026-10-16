#ifndef __SIFT_WRITER_H
#define __SIFT_WRITER_H

#include "sift.h"
#include "sift_format.h"

#include <unordered_map>
#include <array>
#include <vector>
#include <atomic>
#include <fstream>
#include <assert.h>

class vistream;
class vostream;

namespace Sift
{
   /* Code that changes after a writer has sent it.

      Writers send code a page at a time, once (RecOtherIcache), and the
      reader and the simulator cache what they decoded from it by address, so
      code that is rewritten in place -- a JIT, a loader reusing a page, any
      self-modifying program -- would be simulated with the bytes it had when
      its page was first sent.  The binary translators every front end is
      built on (Pin, DynamoRIO, QEMU) re-translate code after it is written,
      so translation is where a change can be seen cheaply: a front end calls
      observe() for each instruction it translates, and when the bytes differ
      from what the writers last sent for that page, the page is logged as
      changed.  Every writer compares one counter per instruction with the
      value it last saw, and only when it moved drops the logged pages, which
      it then sends again -- current bytes -- before the next instruction from
      them.  The reader drops what it decoded from the old bytes.

      One instance per process (the front ends' writers are per thread, the
      code is shared).  Off unless a front end enables it. */
   class CodeTracker
   {
      public:
         static void enable(bool on) { s_enabled = on; }
         static bool enabled() { return s_enabled; }
         // Front end, at translation: the instruction at addr now has these bytes
         static void observe(uint64_t addr, const uint8_t *bytes, uint32_t size);
         // Writers: the number of page changes logged so far
         static uint64_t changes() { return s_changes.load(std::memory_order_acquire); }
         // Writers: the pages logged as changed in [from, to)
         static void changedPages(uint64_t from, uint64_t to, std::vector<uint64_t> &pages);
         // Writers: this page was just sent with these bytes.  Returns the new
         // "seen" value for a writer that had seen `seen` changes, so that a
         // change the writer itself just logged does not make it send again.
         static uint64_t pageSent(uint64_t base, const uint8_t *bytes, uint64_t seen);
         // Statistics, for verbose output and tests
         static uint64_t pagesTracked();
      private:
         static bool s_enabled;
         static std::atomic<uint64_t> s_changes;
   };

   class Writer
   {
      typedef void (*GetCodeFunc)(uint8_t *dst, const uint8_t *src, uint32_t size);
      typedef void (*GetCodeFunc2)(uint8_t *dst, const uint8_t *src, uint32_t size, void *data);
      typedef bool (*HandleAccessMemoryFunc)(void *arg, MemoryLockType lock_signal, MemoryOpType mem_op, uint64_t d_addr, uint8_t *data_buffer, uint32_t data_size);

      private:
         vostream *output;
         vistream *response;
         GetCodeFunc getCodeFunc;
         GetCodeFunc2 getCodeFunc2;
         void *getCodeFunc2Data;
         HandleAccessMemoryFunc handleAccessMemoryFunc;
         void *handleAccessMemoryArg;
         uint64_t ninstrs, hsize[16], haddr[MAX_DYNAMIC_ADDRESSES+1], nbranch, npredicate, ninstrsmall, ninstrext;

         uint64_t last_address;
         std::unordered_map<uint64_t, bool> icache;
         // Per-instruction mode remembers the bytes, not just that an address
         // was seen: self-modifying code reuses an address with new contents,
         // and a seen/not-seen cache can never notice that.
         std::unordered_map<uint64_t, std::array<uint8_t, 16> > icache_bytes;
         int fd_va;
         std::unordered_map<intptr_t, bool> m_va2pa;
         char *m_response_filename;
         uint32_t m_id;
         bool m_requires_icache_per_insn;
         bool m_send_va2pa_mapping;
         uint64_t m_code_changes_seen;   // CodeTracker::changes() when this writer last caught up

         void initResponse();
         void handleMemoryRequest(Record &respRec);
         void send_va2pa(uint64_t va);
         uint64_t va2pa_lookup(uint64_t va);

	 void frontEndStop();
	 /* the simulator closed our channel: nothing is written or read again */
	 void channelLost();

      public:
         Writer(const char *filename, GetCodeFunc getCodeFunc, bool useCompression = false, const char *response_filename = "", uint32_t id = 0, bool arch32 = false, bool requires_icache_per_insn = false, bool send_va2pa_mapping = false, GetCodeFunc2 getCodeFunc2 = NULL, void *GetCodeFunc2Data = NULL);
         ~Writer();
         void End();
         void Instruction(uint64_t addr, uint8_t size, uint8_t num_addresses, uint64_t addresses[], bool is_branch, bool taken, bool is_predicate, bool executed);
         Mode InstructionCount(uint32_t icount);
         void CacheOnly(uint8_t icount, CacheOnlyType type, uint64_t eip, uint64_t address);
         void Output(uint8_t fd, const char *data, uint32_t size);
         uint64_t Syscall(uint16_t syscall_number, const char *data, uint32_t size);
         int32_t NewThread();
         int32_t Join(int32_t);
         Mode Sync();
         uint64_t Magic(uint64_t a, uint64_t b, uint64_t c);
         bool Emulate(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res);
         int32_t Fork();
         void RoutineChange(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip = 0);
         void RoutineAnnounce(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename);
         void ISAChange(uint32_t new_isa);
         // The byte ranges the next Instruction() accesses, when two addresses cannot describe them
         void MemorySpans(uint8_t count, const MemorySpan spans[], uint32_t dropped_lines = 0);
         bool IsOpen();

         void setHandleAccessMemoryFunc(HandleAccessMemoryFunc func, void* arg = NULL) { assert(func); handleAccessMemoryFunc = func; handleAccessMemoryArg = arg; }
   };
};

#endif // __SIFT_WRITER_H
