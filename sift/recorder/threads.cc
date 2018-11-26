#include "threads.h"
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

   // The first thread (master) doesn't need to join with anyone else
   PIN_GetLock(&new_threadid_lock, threadid);
   if (tidptrs.size() > 0)
   {
      thread_data[threadid].tid_ptr = tidptrs.front();
      tidptrs.pop_front();
   }
   PIN_ReleaseLock(&new_threadid_lock);

   thread_data[threadid].thread_num = num_threads++;
   thread_data[threadid].bbv = new Bbv();

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
   if (thread_data[threadid].tid_ptr)
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
