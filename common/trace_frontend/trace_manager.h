#ifndef __TRACE_MANAGER_H
#define __TRACE_MANAGER_H

#include "fixed_types.h"
#include "sniper_semaphore.h"
#include "core.h" // for lock_signal_t and mem_op_t
#include "_thread.h"
#include "sift_reader.h"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <atomic>

class TraceThread;

/* ---------------------------------------------------------------------
 * AppReaderPool
 *
 * A registry mapping an opaque app_thread_id to the Sift::Reader for that
 * user thread's SIFT pipe.  A kernel schedules a user thread onto a core by
 * issuing SimContextSwitchTo(app_thread_id), and the handler looks the
 * reader up here and installs it as that core's app reader.
 *
 * The id is ((app_id << 16) | thread_num), so the id of a new app's main
 * thread can be named before that thread has run -- it is just app_id << 16.
 * ------------------------------------------------------------------- */
class AppReaderPool
{
   public:
      typedef uint64_t app_thread_id_t;

      /* One entry per user thread.  saved_inst / saved_next_inst hold the
         (inst, next_inst) pair the thread was about to execute when it was
         preempted, so a later switch back can resume exactly there;
         has_saved keeps the restore off the path when no preemption
         happened.  The saved-pair fields are written by the preemption path
         and are unused until then. */
      struct Entry {
         Sift::Reader*      reader = nullptr;
         Sift::Instruction  saved_inst;
         Sift::Instruction  saved_next_inst;
         bool               has_saved = false;
      };

      static app_thread_id_t encodeId(app_id_t app_id, UInt32 thread_num) {
         return ((app_thread_id_t)app_id << 16) | (thread_num & 0xffff);
      }

      void registerReader(app_thread_id_t id, Sift::Reader* reader) {
         std::lock_guard<std::mutex> lk(m_lock);
         bool is_new = (m_entries.find(id) == m_entries.end());
         auto& e = m_entries[id];
         e.reader    = reader;
         e.has_saved = false;
         e.saved_inst.sinst = nullptr;
         e.saved_next_inst.sinst = nullptr;
         if (is_new) {
            m_live_count++;
            m_ever_live = true;
         }
      }

      Sift::Reader* lookup(app_thread_id_t id) {
         std::lock_guard<std::mutex> lk(m_lock);
         auto it = m_entries.find(id);
         return it == m_entries.end() ? nullptr : it->second.reader;
      }

      /* A copy of the entry, read under the lock. */
      Entry lookupEntry(app_thread_id_t id) {
         std::lock_guard<std::mutex> lk(m_lock);
         auto it = m_entries.find(id);
         return it == m_entries.end() ? Entry{} : it->second;
      }

      void stashSavedPair(app_thread_id_t id,
                          const Sift::Instruction& inst,
                          const Sift::Instruction& next_inst) {
         std::lock_guard<std::mutex> lk(m_lock);
         auto it = m_entries.find(id);
         if (it == m_entries.end()) return;
         it->second.saved_inst = inst;
         it->second.saved_next_inst = next_inst;
         it->second.has_saved = true;
      }

      void clearSavedPair(app_thread_id_t id) {
         std::lock_guard<std::mutex> lk(m_lock);
         auto it = m_entries.find(id);
         if (it == m_entries.end()) return;
         it->second.has_saved = false;
         it->second.saved_inst.sinst = nullptr;
         it->second.saved_next_inst.sinst = nullptr;
      }

      /* Drop an entry and tell its front end to stop, so a SIFT Writer
         blocked waiting for a response unblocks rather than failing on a
         short read at teardown.  The Reader itself is not deleted: readers
         are assumed to live until process exit, and one may still be
         referenced by a TraceThread on another core. */
      void unregisterReader(app_thread_id_t id) {
         Sift::Reader* reader = nullptr;
         {
            std::lock_guard<std::mutex> lk(m_lock);
            auto it = m_entries.find(id);
            if (it == m_entries.end()) return;
            reader = it->second.reader;
            m_entries.erase(it);
            if (m_live_count > 0) m_live_count--;
         }
         if (reader) {
            reader->frontEndStop();
         }
      }

      /* True exactly once, when the last registered thread has gone after
         at least one was live -- the signal that the simulation can end.
         Later calls return false even though the count is still zero. */
      bool wasEverLiveAndNowZero() {
         std::lock_guard<std::mutex> lk(m_lock);
         if (m_ever_live && m_live_count == 0 && !m_terminated) {
            m_terminated = true;
            return true;
         }
         return false;
      }

      size_t size() {
         std::lock_guard<std::mutex> lk(m_lock);
         return m_entries.size();
      }

      /* Stop every registered front end, for teardown. */
      void stopAll() {
         std::lock_guard<std::mutex> lk(m_lock);
         for (auto& kv : m_entries) {
            if (kv.second.reader) kv.second.reader->frontEndStop();
         }
      }

   private:
      std::unordered_map<app_thread_id_t, Entry> m_entries;
      std::mutex m_lock;
      size_t m_live_count  = 0;
      bool   m_ever_live   = false;
      bool   m_terminated  = false;
};

class TraceManager
{
   private:
      class Monitor : public Runnable
      {
         private:
            void run();
            _Thread *m_thread;
            TraceManager *m_manager;
         public:
            Monitor(TraceManager *manager);
            ~Monitor();
            void spawn();
      };

      struct app_info_t
      {
         app_info_t()
            : thread_count(1)
            , num_threads(1)
            , num_runs(0)
         {}
         UInt32 thread_count;       //< Index counter for new thread's FIFO name
         UInt32 num_threads;        //< Number of active threads for this app (when zero, app is done)
         UInt32 num_runs;           //< Number of completed runs
      };

      Monitor *m_monitor;
      std::vector<TraceThread *> m_threads;
      UInt32 m_num_threads_started;
      UInt32 m_num_threads_running;
      Semaphore m_done;
      const bool m_stop_with_first_app;
      const bool m_app_restart;
      const bool m_emulate_syscalls;
      UInt32 m_num_apps;
      UInt32 m_num_apps_nonfinish;  //< Number of applications that have yet to complete their first run
      std::vector<app_info_t> m_app_info;
      std::vector<String> m_tracefiles;
      std::vector<String> m_responsefiles;
      String m_trace_prefix;
      Lock m_lock;

      Sift::Reader *m_kernel_trace_reader;

      /* Readers for injected applications, keyed by app_thread_id. */
      AppReaderPool m_app_reader_pool;

      /* Set once stop() has been entered.  stop() is reachable from more
         than one place -- the end of a run, and a magic handler deciding the
         last application has finished -- and running it twice writes to
         response pipes that the first pass already closed. */
      std::atomic<bool> m_shutdown_requested{false};

      String getFifoName(app_id_t app_id, UInt64 thread_num, bool response, bool create);
      thread_id_t newThread(app_id_t app_id, bool first, bool init_fifo, bool spawn, SubsecondTime time, thread_id_t creator_thread_id);

      friend class Monitor;

   public:
      /* Public because the scheduling path re-binds a reader's handlers to
         whichever TraceThread is about to read it -- see
         SIM_CMD_CONTEXT_SWITCH_TO. */
      void setTraceReaderHandlers(Sift::Reader* reader, TraceThread* trace_thread);

      TraceManager();
      ~TraceManager();
      void init();
      void start();
      void stop();
      void mark_done();
      void wait();
      void run();
      void cleanup();
      void cleanupAllThreads();  // Clean up ChampSim caches on all trace threads
      void setupTraceFiles(int index);
      thread_id_t createThread(app_id_t app_id, SubsecondTime time, thread_id_t creator_thread_id);
      app_id_t createApplication(SubsecondTime time, thread_id_t creator_thread_id);
      app_id_t createTraceBasedApplication(SubsecondTime time, char* trace, thread_id_t creator_thread_id);
      void signalStarted();
      void signalDone(TraceThread *thread, SubsecondTime time, bool aborted);
      void endApplication(TraceThread *thread, SubsecondTime time);
      void accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);
      void endFrontEnd(); //Ask all trace_threads to send signal to front-end to shutdown

      TraceThread *getTraceThread(app_id_t app_id, thread_id_t th);

      AppReaderPool &getAppReaderPool() { return m_app_reader_pool; }

      /* True once shutdown has begun.  A handler that waits for an event has
         to stop waiting when no more will arrive. */
      bool isDone() const
         { return m_shutdown_requested.load(std::memory_order_acquire); }

      Sift::Reader *getKernelTraceReader() const { return m_kernel_trace_reader; }
      void setKernelTraceReader(Sift::Reader *reader) { m_kernel_trace_reader = reader; }
      
      UInt64 getProgressExpect();
      UInt64 getProgressValue();
};

#endif // __TRACE_MANAGER_H
