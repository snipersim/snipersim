#ifndef __TRACE_THREAD_H
#define __TRACE_THREAD_H

#include "fixed_types.h"
#include "barrier.h"
#include "_thread.h"
#include "thread.h"
#include "core.h"
#include "sift_reader.h"

#include <unordered_map>

class BasicBlock;

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

      UInt64 va2pa(UInt64 va) { return (UInt64(m_thread->getId()) << pa_core_shift) | (va & pa_va_mask); }

      _Thread *m__thread;
      Thread *m_thread;
      Sift::Reader m_trace;
      bool m_stop;
      Barrier *m_barrier;
      std::unordered_map<IntPtr, BasicBlock *> m_icache;
      IntPtr m_bbv_base;
      UInt64 m_bbv_count;
      xed_syntax_enum_t m_syntax;
      uint8_t m_output_leftover[160];
      uint16_t m_output_leftover_size;
      String m_tracefile;
      String m_responsefile;
      app_id_t m_app_id;
      bool m_cleanup;

      void run();
      static void __handleOutputFunc(void* arg, uint8_t fd, const uint8_t *data, uint32_t size)
      { ((TraceThread*)arg)->handleOutputFunc(fd, data, size); }
      static uint64_t __handleSyscallFunc(void* arg, uint16_t syscall_number, const uint8_t *data, uint32_t size)
      { return ((TraceThread*)arg)->handleSyscallFunc(syscall_number, data, size); }
      static int32_t __handleNewThreadFunc(void* arg)
      { return ((TraceThread*)arg)->handleNewThreadFunc(); }
      static int32_t __handleJoinFunc(void* arg, int32_t join_thread_id)
      { return ((TraceThread*)arg)->handleJoinFunc(join_thread_id); }
      void handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size);
      uint64_t handleSyscallFunc(uint16_t syscall_number, const uint8_t *data, uint32_t size);
      int32_t handleNewThreadFunc();
      int32_t handleJoinFunc(int32_t thread);
      BasicBlock* decode(Sift::Instruction &inst);

   public:
      TraceThread(Thread *thread, String tracefile, String responsefile, app_id_t app_id, bool cleanup);
      ~TraceThread();

      void spawn(Barrier *barrier);
      void stop() { m_stop = true; }
      UInt64 getProgressExpect();
      UInt64 getProgressValue();
      Thread* getThread() const { return m_thread; }
      void handleAccessMemory(Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);
};

#endif // __TRACE_THREAD_H
