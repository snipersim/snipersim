#include "sift_assert.h"
#include <iostream>
#include <unistd.h>
#include <syscall.h>
#include <linux/futex.h>
//#include "frontend_threads.h"

namespace frontend
{

// declare static data to be able to use it here
template <typename T>
thread_data_t* FrontendThreads<T>::m_thread_data;
template <typename T>
uint32_t* FrontendThreads<T>::p_num_threads;
template <typename T>
FELock<T>* FrontendThreads<T>::new_threadid_lock;
template <typename T>
std::deque<uint64_t>* FrontendThreads<T>::tidptrs;
template <typename T>
FrontendControl<T>* FrontendThreads<T>::m_control;
template <typename T>
FrontendOptions<T>* FrontendThreads<T>::m_options;

template <typename T> 
FrontendThreads<T>::FrontendThreads(FrontendOptions<T>* opts, FrontendControl<T>* control, thread_data_t* td, 
                                    uint32_t* pnt, FELock<T>* ntid_lock, std::deque<uint64_t>* tids)
{
  m_options = opts;
  new_threadid_lock = ntid_lock;
  m_thread_data = td;
  p_num_threads = pnt;
  tidptrs = tids;
  m_control = control;
}

template <typename T> 
FrontendThreads<T>::~FrontendThreads()
{

}
 
// The thread that watched this new thread start is responsible for setting up the connection with the simulator
template <typename T>
void FrontendThreads<T>::threadStart(threadid_t threadid)
{     
  sift_assert(m_thread_data[threadid].bbv == NULL);

  // The first thread (master) doesn't need to join with anyone else
  new_threadid_lock->acquire_lock(threadid);
  if (tidptrs->size() > 0)
  {
    m_thread_data[threadid].tid_ptr = tidptrs->front();
    tidptrs->pop_front();
  }
  new_threadid_lock->release_lock();
 
  m_thread_data[threadid].thread_num = (*p_num_threads)++;
  m_thread_data[threadid].bbv = new Bbv();

  if (threadid > 0 && (m_control->get_any_thread_in_detail() || m_options->get_emulate_syscalls()))
  {
    m_control->openFile(threadid);

    // We should send a EmuTypeSetThreadInfo, but not now as we hold the Pin VM lock:
    // Sending EmuTypeSetThreadInfo requires the response channel to be opened,
    // which is done by TraceThread but not any time soon if we aren't scheduled on a core.
    m_thread_data[threadid].should_send_threadinfo = true;
  }

  m_thread_data[threadid].running = true;
}

template <typename T>
void FrontendThreads<T>::threadFinishHelper(void *arg)
{
   uint64_t threadid = reinterpret_cast<uint64_t>(arg);
   if (m_thread_data[threadid].tid_ptr)
   {
      // Set this pointer to 0 to indicate that this thread is complete
      intptr_t tid = (intptr_t)m_thread_data[threadid].tid_ptr;
      *(int*)tid = 0;
      // Send the FUTEX_WAKE to the simulator to wake up a potential pthread_join() caller
      syscall_args_t args = {0};
      args[0] = (intptr_t)tid;
      args[1] = FUTEX_WAKE;
      args[2] = 1;

      m_thread_data[threadid].output->Syscall(SYS_futex, (char*)args, sizeof(args));
   }

   if (m_thread_data[threadid].output)
   {
      m_control->closeFile(threadid);
   }

   delete m_thread_data[threadid].bbv;

   m_thread_data[threadid].bbv = NULL;
}

template <typename T>
void FrontendThreads<T>::threadFinish(threadid_t threadid, int32_t flags)
{
   //std::cerr << "[FRONTEND:" << app_id << ":" << m_thread_data[threadid].thread_num << "] Finish Thread" << std::endl;  // TODO app_id not accessible

   if (m_thread_data[threadid].thread_num == 0 && m_thread_data[threadid].output && m_options->get_emulate_syscalls())
   {
      // Send SYS_exit_group to the simulator to end the application
      syscall_args_t args = {0};
      args[0] = flags;
      m_thread_data[threadid].output->Syscall(SYS_exit_group, (char*)args, sizeof(args));
   }

   m_thread_data[threadid].running = false;

   // To prevent deadlocks during simulation, start a new thread to handle this thread's
   // cleanup.  This is needed because this function could be called in the context of
   // another thread, creating a deadlock scenario.
   //PIN_SpawnInternalThread(threadFinishHelper, (void*)(unsigned long)threadid, 0, NULL);
   callFinishHelper(threadid);
}

template <typename T>
void FrontendThreads<T>::callFinishHelper(threadid_t threadid)
{
}

template <typename T>
void FrontendThreads<T>::initThreads()
{
}

} // namespace frontend
