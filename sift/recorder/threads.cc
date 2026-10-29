#include "threads.h"
#include "timed_calls.h"
#include "sift_assert.h"
#include "recorder_control.h"

#include <iostream>
#include <unistd.h>
#include <syscall.h>
#include <linux/futex.h>

// Enable the below to print out sizeof(thread_data_t) at compile time
#if 0
// http://stackoverflow.com/questions/7931358/printing-sizeoft-at-compile-time
template<int N>
struct _{ operator char() { return N+ 256; } }; //always overflow
void print_size() { char(_<sizeof(thread_data_t)>()); }
#endif

static_assert((sizeof(thread_data_t) % LINE_SIZE_BYTES) == 0, "Error: Thread data should be a multiple of the line size to prevent false sharing");

thread_data_t *thread_data;

// The thread that watched this new thread start is responsible for setting up the connection with the simulator
static VOID threadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
   sift_assert(threadid < max_num_threads);
   sift_assert(thread_data[threadid].bbv == NULL);

   // The first thread (master) doesn't need to join with anyone else.
   //
   // Every clone() of a thread queued its clear-tid word, the one
   // pthread_join waits on.  Take this thread's own, not the oldest: threads
   // cloned back to back reach this callback in either order, and a thread
   // holding its sibling's word clears it and wakes the sibling's
   // pthread_join when it exits -- before the sibling has run, when the
   // scheduler holds the sibling back (the sequential scheduler queues it for
   // its core), so the joiner reads results that were never written.  The
   // clone passed the child's TLS base (CLONE_SETTLS), which is the FS base
   // the child starts with: that identifies the entry exactly.
   INT32 parent = -1;
   PIN_GetLock(&new_threadid_lock, threadid);
   if (tidptrs.size() > 0)
   {
#if defined(TARGET_INTEL64) || defined(TARGET_IA32E)
      ADDRINT fs_base = PIN_GetContextReg(ctxt, REG_SEG_FS_BASE);
#else
      ADDRINT fs_base = 0;
#endif
      auto it = tidptrs.begin();
      for (; it != tidptrs.end(); ++it)
         if (it->tls && it->tls == fs_base)
            break;
      if (it == tidptrs.end())
         it = tidptrs.begin();   // no TLS to match on: the oldest-first pairing
      thread_data[threadid].tid_ptr = it->tid_ptr;
      parent = it->parent;
      tidptrs.erase(it);
   }
   PIN_ReleaseLock(&new_threadid_lock);

   thread_data[threadid].thread_num = num_threads++;
   thread_data[threadid].icount_total = 0;
   thread_data[threadid].bbv = new Bbv();
   timedThreadStart(threadid, parent);   /* its clock, from its parent's time */

   if (threadid > 0 && (any_thread_in_detail || KnobEmulateSyscalls.Value()))
   {
      openFile(threadid);

      // We should send a EmuTypeSetThreadInfo, but not now as we hold the Pin VM lock:
      // Sending EmuTypeSetThreadInfo requires the response channel to be opened,
      // which is done by TraceThread but not any time soon if we aren't scheduled on a core.
      thread_data[threadid].should_send_threadinfo = true;
   }

   thread_data[threadid].running = true;
}

static VOID threadFinishHelper(VOID *arg)
{
   uint64_t threadid = reinterpret_cast<uint64_t>(arg);
   /* live only: without a simulator the kernel clears the word and wakes the
      joiner itself */
   if (thread_data[threadid].tid_ptr && thread_data[threadid].output && KnobEmulateSyscalls.Value())
   {
      // Set this pointer to 0 to indicate that this thread is complete
      intptr_t tid = (intptr_t)thread_data[threadid].tid_ptr;
      *(int*)tid = 0;
      // Send the FUTEX_WAKE to the simulator to wake up a potential pthread_join() caller
      syscall_args_t args = {0};
      args[0] = (intptr_t)tid;
      args[1] = FUTEX_WAKE;
      args[2] = 1;
      thread_data[threadid].output->Syscall(SYS_futex, (char*)args, sizeof(args));
   }

   if (thread_data[threadid].output)
   {
      closeFile(threadid);
   }

   delete thread_data[threadid].bbv;

   thread_data[threadid].bbv = NULL;
}

static VOID threadFinish(THREADID threadid, const CONTEXT *ctxt, INT32 flags, VOID *v)
{
#if DEBUG_OUTPUT
   std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Finish Thread" << std::endl;
#endif

   if (thread_data[threadid].thread_num == 0 && thread_data[threadid].output && KnobEmulateSyscalls.Value())
   {
      // Send SYS_exit_group to the simulator to end the application
      syscall_args_t args = {0};
      args[0] = flags;
      thread_data[threadid].output->Syscall(SYS_exit_group, (char*)args, sizeof(args));
   }

   thread_data[threadid].running = false;
   timedThreadExit(threadid);

   // To prevent deadlocks during simulation, start a new thread to handle this thread's
   // cleanup.  This is needed because this function could be called in the context of
   // another thread, creating a deadlock scenario.
   PIN_SpawnInternalThread(threadFinishHelper, (VOID*)(unsigned long)threadid, 0, NULL);
}

void initThreads()
{
   PIN_AddThreadStartFunction(threadStart, 0);
   PIN_AddThreadFiniFunction(threadFinish, 0);
}
