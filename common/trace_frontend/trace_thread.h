#ifndef __TRACE_THREAD_H
#define __TRACE_THREAD_H

#include "fixed_types.h"
#include "_thread.h"
#include "thread.h"
#include "core.h"
#include "sift_reader.h"
#include "riscv_isa.h"
#include "trace_manager.h"   // AppReaderPool
#include "operand.h"
#include "sniper_semaphore.h"

#include <decoder.h>

#include <unordered_map>
#include <atomic>
#include <unistd.h>
#include <sys/syscall.h>

#define NUM_PAPI_COUNTERS 6

#define PAPI_TOT_INS 0
#define PAPI_TOT_CYC 1
#define PAPI_L1_DCM 2
#define PAPI_L2_DCM 3
#define PAPI_L3_TCM 4
#define PAPI_BR_MSP 5

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
      Sift::Reader *m_kernel_trace;          // SIFT reader for the kernel trace, if any
      Sift::Reader *m_app_trace;             // SIFT reader for the application trace, if any
      Sift::Reader *m_current_sift_reader;   // Currently active SIFT reader

      bool is_in_kernel_mode;

      bool m_trace_has_pa;
      bool m_champsim_trace;
      UInt32 m_champsim_access_size;
      bool m_address_randomization;
      bool m_appid_from_coreid;
      uint8_t m_address_randomization_table[256];
      bool m_stop;
      // Keyed by the reader's static instruction, not its address: code that
      // is rewritten in place gives a new static instruction at the same
      // address, and so does another reader (the kernel's and the
      // application's streams under userspace MimicOS) on this thread.
      std::unordered_map<const Sift::StaticInstruction *, Instruction *> m_icache;
      std::unordered_map<const Sift::StaticInstruction *, const dl::DecodedInst *> m_decoder_cache;
      
      // ChampSim instruction cache: key is (PC, is_branch, num_src_regs, num_dest_regs, num_loads, num_stores)
      // This allows reusing instructions when the same PC has the same operand signature
      struct ChampSimCacheKey {
         IntPtr pc;
         uint8_t is_branch;
         uint8_t num_src_regs;
         uint8_t num_dest_regs;
         uint8_t num_loads;
         uint8_t num_stores;
         
         bool operator==(const ChampSimCacheKey& other) const {
            return pc == other.pc && is_branch == other.is_branch &&
                   num_src_regs == other.num_src_regs && num_dest_regs == other.num_dest_regs &&
                   num_loads == other.num_loads && num_stores == other.num_stores;
         }
      };
      struct ChampSimCacheKeyHash {
         size_t operator()(const ChampSimCacheKey& k) const {
            // Combine all fields into a hash
            return std::hash<IntPtr>()(k.pc) ^ 
                   (std::hash<uint8_t>()(k.is_branch) << 1) ^
                   (std::hash<uint8_t>()(k.num_src_regs) << 2) ^
                   (std::hash<uint8_t>()(k.num_dest_regs) << 3) ^
                   (std::hash<uint8_t>()(k.num_loads) << 4) ^
                   (std::hash<uint8_t>()(k.num_stores) << 5);
         }
      };
      std::unordered_map<ChampSimCacheKey, Instruction*, ChampSimCacheKeyHash> m_champsim_icache;
      
      UInt64 m_bbv_base;
      UInt64 m_bbv_count;
      UInt64 m_instructions_retired;   // the thread's instructions in every mode, but m_bbv_count's
      UInt64 m_bbv_last;
      bool m_bbv_end;
      static int m_isa;
      uint8_t m_output_leftover[160];
      uint16_t m_output_leftover_size;
      String m_tracefile;
      String m_responsefile;

      String m_tracefile_kernel;
      String m_responsefile_kernel;
      
      app_id_t m_app_id;
      /* The application injected into this TraceThread, or -1 when none is.
         A kernel asks for this straight after injecting one and derives the
         thread id as app_id << 16, which is how the pool is keyed. */
      app_id_t m_injected_app_id = (app_id_t)-1;

      /* The user thread this TraceThread is currently running, as a pool id,
         or 0 when none has been scheduled onto it. */
      AppReaderPool::app_thread_id_t m_running_app_tid = 0;

      /* Scheduling quantum, counted in detailed application instructions.
         When m_instrs_this_quantum reaches m_quantum_instrs the thread has
         used its slice and a quantum_expired event is posted to its core, so
         a kernel can rotate its runqueue.  Configured by
         scheduler/quantum_instructions. */
      uint64_t m_quantum_instrs      = 10000;
      uint64_t m_instrs_this_quantum = 0;
      uint64_t m_quanta_expired      = 0;   //< for the statistic below
      bool m_blocked;

   public:
      /* Where this thread is, for the hang report (traceinput/hang_report):
         set on entry to each front-end callback and around trace reads,
         restored on exit, so a report names the innermost one.  A relaxed
         pointer store, cheap enough to leave on. */
      std::atomic<const char *> m_where{"starting"};
      std::atomic<int64_t> m_where_arg{-1};
      std::atomic<long> m_os_tid{0};   // the host thread running this one, for the hang report
      struct Where
      {
         TraceThread *t; const char *prev; int64_t prev_arg;
         Where(TraceThread *t_, const char *w, int64_t arg = -1)
            : t(t_), prev(t_->m_where.load(std::memory_order_relaxed)), prev_arg(t_->m_where_arg.load(std::memory_order_relaxed))
         { t->m_where.store(w, std::memory_order_relaxed); t->m_where_arg.store(arg, std::memory_order_relaxed); }
         ~Where() { t->m_where.store(prev, std::memory_order_relaxed); t->m_where_arg.store(prev_arg, std::memory_order_relaxed); }
      };
      bool isBlockedInSyscall() const { return m_blocked; }
      bool isStopRequested() const { return m_stop; }
   private:
      bool m_cleanup;
      bool m_started;

   public:
      bool m_stopped;
      /* set last thing before the reader's function returns: the thread is
         out of the simulator, and teardown may proceed as far as it is
         concerned (TraceManager::wait) */
      volatile bool m_finished;
      bool started() const { return m_started; }
   private:

      // ChampSim instruction cache statistics
      UInt64 m_champsim_icache_hits;
      UInt64 m_champsim_icache_misses;
      // RISC-V: instructions per class of the generated decoder (riscv_isa.h) and recorded footprints
      UInt64 m_riscv_count[64];
      UInt64 m_riscv_spans;
      UInt64 m_riscv_span_bytes;
      UInt64 m_riscv_spans_dropped_lines;   /* lines touched beyond the spans a record holds */
      bool m_count_riscv;
      uint32_t staticMemoryOperands(const Sift::Instruction &inst, const dl::DecodedInst &dec_inst);
      void countExtension(const Sift::Instruction &inst, const dl::DecodedInst &dec_inst);

      int fd_read;
      int fd_write;

      // INVARIANT: to_be_replayed_inst.sinst != NULL iff thread->getCore()->getMemoryManager()->is_page_fault = True
      Sift::Instruction  to_be_replayed_inst;      // This is the instruction that is currently being replayed, if any
      Sift::Instruction  to_be_replayed_next_inst; 

      struct statistics
      {
         SubsecondTime kernel_time;
      } stats;
      // Make run() a function pointer which is initialized in the constructor
      typedef void (TraceThread::*RunFunc)();
      RunFunc m_current_run_func;
      
      void run(){
         m_os_tid.store((long)syscall(SYS_gettid), std::memory_order_relaxed);
         (this->*m_current_run_func)();
      }

      void m_run_func_default();
      void m_run_func_with_userpace_mimicos();
      bool accountQuantumInstruction();   // true when this instruction ended the quantum
      void notifyAppExitAndSwitchToKernel();

      Sift::Mode handleInstructionCountFunc(uint32_t icount);
      void handleCacheOnlyFunc(uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address);
      void handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size);
      uint64_t handleSyscallFunc(uint16_t syscall_number, const uint8_t *data, uint32_t size);
      int32_t handleNewThreadFunc();
      int32_t handleForkFunc();
      int32_t handleJoinFunc(int32_t thread);
      uint64_t handleMagicFunc(uint64_t a, uint64_t b, uint64_t c, bool *respond);
      /* The trace's ISA against the configured core's (general/arch), each
         time the trace's changes: an x86 trace on an ARM core either aborted
         inside the ARM decoder or, in fast-forward, which decodes nothing,
         ran as if all were well. */
      void checkIsa(const Sift::Instruction &inst)
      { if (inst.isa != m_isa_checked) checkIsaChange(inst.isa); }
      void checkIsaChange(int isa);
      int m_isa_checked = -1;
      bool handleEmuFunc(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res);
      void handleRoutineChangeFunc(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip);
      void handleRoutineAnnounceFunc(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename);



      Instruction* decode(Sift::Instruction &inst);
      Instruction* decodeChampsim(Sift::Instruction &inst);
      void handleInstructionWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core, bool do_icache_warmup, UInt64 icache_warmup_addr, UInt64 icache_warmup_size);
      void handleChampSimWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core);
      void handleInstructionDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl);
      void handleChampSimDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl);
      void addDetailedMemoryInfo(DynamicInstruction *dynins, Sift::Instruction &inst, const dl::DecodedInst &decoded_inst, uint32_t mem_idx, Operand::Direction op_type, bool is_pretetch, PerformanceModel *prfmdl);
      void addChampSimMemoryInfo(DynamicInstruction *dynins, UInt64 mem_address, Operand::Direction op_type, bool executed, bool is_prefetch, PerformanceModel *prfmdl);
      void unblock();

      SubsecondTime getCurrentTime() const;
      

      dl::DecoderFactory *m_factory;  // we need a factory here to be able to create instructions of any kind
      const dl::DecodedInst* staticDecode(Sift::Instruction &inst);

      long long *m_papi_counters;
      bool m_virtuos_app;
      
      Lock m_lock;

   public:

         static Sift::Mode __handleInstructionCountFunc(void* arg, uint32_t icount)
      { Where w((TraceThread*)arg, "instruction-count callback"); return ((TraceThread*)arg)->handleInstructionCountFunc(icount); }
      static void __handleCacheOnlyFunc(void* arg, uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address)
      { ((TraceThread*)arg)->handleCacheOnlyFunc(icount, type, eip, address); }
      static void __handleOutputFunc(void* arg, uint8_t fd, const uint8_t *data, uint32_t size)
      { ((TraceThread*)arg)->handleOutputFunc(fd, data, size); }
      static uint64_t __handleSyscallFunc(void* arg, uint16_t syscall_number, const uint8_t *data, uint32_t size)
      { Where w((TraceThread*)arg, "syscall", syscall_number); return ((TraceThread*)arg)->handleSyscallFunc(syscall_number, data, size); }
      static int32_t __handleNewThreadFunc(void* arg)
      { Where w((TraceThread*)arg, "new-thread callback"); return ((TraceThread*)arg)->handleNewThreadFunc(); }
      static int32_t __handleJoinFunc(void* arg, int32_t join_thread_id)
      { Where w((TraceThread*)arg, "join callback", join_thread_id); return ((TraceThread*)arg)->handleJoinFunc(join_thread_id); }
      static uint64_t __handleMagicFunc(void* arg, uint64_t a, uint64_t b, uint64_t c, bool *respond)
      { Where w((TraceThread*)arg, "magic callback", (int64_t)a); return ((TraceThread*)arg)->handleMagicFunc(a, b, c, respond); }
      static bool __handleEmuFunc(void* arg, Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res)
      { Where w((TraceThread*)arg, "emulation callback", (int64_t)type); return ((TraceThread*)arg)->handleEmuFunc(type, req, res); }
      static void __handleRoutineChangeFunc(void* arg, Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip)
      { ((TraceThread*)arg)->handleRoutineChangeFunc(event, eip, esp, callEip); }
      static void __handleRoutineAnnounceFunc(void* arg, uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
      { ((TraceThread*)arg)->handleRoutineAnnounceFunc(eip, name, imgname, offset, line, column, filename); }
      static int32_t __handleForkFunc(void* arg)
      { return ((TraceThread*)arg)->handleForkFunc();}

      TraceThread(Thread *thread, SubsecondTime time_start, String tracefile, String responsefile, app_id_t app_id, bool cleanup);
      ~TraceThread();

      void spawn();
      void stop() { m_stop = true; }
      void cleanupChampSimCache();  // Clean up ChampSim instruction cache to avoid leaks
      UInt64 getProgressExpect();
      UInt64 getProgressValue();
      void frontEndStop(); //Ask all trace_threads to send signal to front-end to shutdown
      void suppressShutdown(); // the end of the run must not stop this thread's front end (its application ended earlier)

      Thread* getThread() const { return m_thread; }
      bool getVirtuosApp() { return m_virtuos_app; }
     
      void handleAccessMemory(Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);
      
      Sift::Reader* getSiftReader() { return &m_trace; }
      
      Sift::Reader* getKernelSiftReader() { return m_kernel_trace; }
      void setKernelSIFTRreader(Sift::Reader *reader) { m_kernel_trace = reader; }
         
      Sift::Reader* getAppSiftReader() { return m_app_trace; }
      void setAppSiftReader(Sift::Reader *reader) { m_app_trace = reader; }

      void setInjectedAppId(app_id_t id) { m_injected_app_id = id; }
      void setRunningAppTid(AppReaderPool::app_thread_id_t id)
         { m_running_app_tid = id; m_instrs_this_quantum = 0; }
      AppReaderPool::app_thread_id_t getRunningAppTid() const
         { return m_running_app_tid; }
      app_id_t getInjectedAppId() const { return m_injected_app_id; }


      Sift::Reader* getCurrentSiftReader() { return m_current_sift_reader; }
      void setCurrentSiftReader(Sift::Reader *reader) { m_current_sift_reader = reader; }
};

#endif // __TRACE_THREAD_H
