#ifndef __TRACE_THREAD_H
#define __TRACE_THREAD_H

#include "fixed_types.h"
#include "_thread.h"
#include "thread.h"
#include "core.h"
#include "sift_reader.h"
#include "operand.h"

#include <unordered_map>

class Instruction;
class DynamicInstruction;

class TraceThread : public Runnable
{
   private:
      // In multi-process mode, we want each process to have its own private memory space
      // Therefore, perform a virtual to physical address mapping by including the core_id
      // Virtual addresses are converted to physical addresses by pasting the core_id at
      // bit positions pa_core_shift..+pa_core_size
      // The highest virtual address used is normally 00007fffffffffff, with pa_core_shift==48
      // the core_id is 2 above the highest used bit.
      static const UInt64 pa_core_shift = 48;
      static const UInt64 pa_core_size = 16;
      static const UInt64 pa_va_mask = ~(((UInt64(1) << pa_core_size) - 1) << pa_core_shift);
      // Optionally we can also do address randomization on a per-page basis.
      // This can avoid artificial set contention when replaying multiple copies of the same trace.
      static const UInt64 va_page_shift = 12;
      static const UInt64 va_page_mask = (UInt64(1) << va_page_shift) - 1;

      static UInt64 _va2pa(UInt64 self, UInt64 va) { return ((TraceThread*)self)->va2pa(va); }
      UInt64 va2pa(UInt64 va, bool *noMapping = NULL);
      UInt64 remapAddress(UInt64 va_page);

      _Thread *m__thread;
      Thread *m_thread;
      SubsecondTime m_time_start;
      Sift::Reader m_trace;
      bool m_trace_has_pa;
      bool m_address_randomization;
      uint8_t m_address_randomization_table[256];
      bool m_stop;
      std::unordered_map<IntPtr, Instruction *> m_icache;
      UInt64 m_bbv_base;
      UInt64 m_bbv_count;
      UInt64 m_bbv_last;
      bool m_bbv_end;
      xed_syntax_enum_t m_syntax;
      uint8_t m_output_leftover[160];
      uint16_t m_output_leftover_size;
      String m_tracefile;
      String m_responsefile;
      app_id_t m_app_id;
      bool m_blocked;
      bool m_cleanup;
      bool m_started;

      void run();
      static Sift::Mode __handleInstructionCountFunc(void* arg, uint32_t icount)
      { return ((TraceThread*)arg)->handleInstructionCountFunc(icount); }
      static void __handleCacheOnlyFunc(void* arg, uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address)
      { ((TraceThread*)arg)->handleCacheOnlyFunc(icount, type, eip, address); }
      static void __handleOutputFunc(void* arg, uint8_t fd, const uint8_t *data, uint32_t size)
      { ((TraceThread*)arg)->handleOutputFunc(fd, data, size); }
      static uint64_t __handleSyscallFunc(void* arg, uint16_t syscall_number, const uint8_t *data, uint32_t size)
      { return ((TraceThread*)arg)->handleSyscallFunc(syscall_number, data, size); }
      static int32_t __handleNewThreadFunc(void* arg)
      { return ((TraceThread*)arg)->handleNewThreadFunc(); }
      static int32_t __handleJoinFunc(void* arg, int32_t join_thread_id)
      { return ((TraceThread*)arg)->handleJoinFunc(join_thread_id); }
      static uint64_t __handleMagicFunc(void* arg, uint64_t a, uint64_t b, uint64_t c)
      { return ((TraceThread*)arg)->handleMagicFunc(a, b, c); }
      static bool __handleEmuFunc(void* arg, Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res)
      { return ((TraceThread*)arg)->handleEmuFunc(type, req, res); }
      static void __handleRoutineChangeFunc(void* arg, Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip)
      { ((TraceThread*)arg)->handleRoutineChangeFunc(event, eip, esp, callEip); }
      static void __handleRoutineAnnounceFunc(void* arg, uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
      { ((TraceThread*)arg)->handleRoutineAnnounceFunc(eip, name, imgname, offset, line, column, filename); }
      static int32_t __handleForkFunc(void* arg)
      { return ((TraceThread*)arg)->handleForkFunc();}

      Sift::Mode handleInstructionCountFunc(uint32_t icount);
      void handleCacheOnlyFunc(uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address);
      void handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size);
      uint64_t handleSyscallFunc(uint16_t syscall_number, const uint8_t *data, uint32_t size);
      int32_t handleNewThreadFunc();
      int32_t handleForkFunc();
      int32_t handleJoinFunc(int32_t thread);
      uint64_t handleMagicFunc(uint64_t a, uint64_t b, uint64_t c);
      bool handleEmuFunc(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res);
      void handleRoutineChangeFunc(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip);
      void handleRoutineAnnounceFunc(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename);

      Instruction* decode(Sift::Instruction &inst);
      void handleInstructionWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core, bool do_icache_warmup, UInt64 icache_warmup_addr, UInt64 icache_warmup_size);
      void handleInstructionDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl);
      void addDetailedMemoryInfo(DynamicInstruction *dynins, Sift::Instruction &inst, const xed_decoded_inst_t &xed_inst, uint32_t mem_idx, Operand::Direction op_type, bool is_pretetch, PerformanceModel *prfmdl);
      void unblock();

      SubsecondTime getCurrentTime() const;

   public:
      bool m_stopped;

      TraceThread(Thread *thread, SubsecondTime time_start, String tracefile, String responsefile, app_id_t app_id, bool cleanup);
      ~TraceThread();

      void spawn();
      void stop() { m_stop = true; }
      UInt64 getProgressExpect();
      UInt64 getProgressValue();
      Thread* getThread() const { return m_thread; }
      void handleAccessMemory(Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);
};

#endif // __TRACE_THREAD_H
