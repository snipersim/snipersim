#ifndef __FRONTEND_THREADS_H
#define __FRONTEND_THREADS_H

#include "sift_format.h"
#include "frontend_defs.h"
#include "sift_writer.h"
#include "bbv_count.h"
#include "frontend_utils.h"
#include "frontend_control.h"
#include "frontend_options.h"
#include <deque>


namespace frontend
{

/**
 * @class FrontendThreads
 *
 * Class that implements methods related to frontend threads
 * T: Frontend type
 */
template <typename T> class FrontendThreads
{
public:
  FrontendThreads(FrontendOptions<T>* opts, FrontendControl<T>* control, thread_data_t* td, uint32_t* pnt, 
                  FELock<T>* ntid_lock, std::deque<uint64_t>* tids);
  ~FrontendThreads();
  void initThreads();
  static void threadStart(threadid_t threadid);
  static void threadFinish(threadid_t threadid, int32_t flags);
  static void callFinishHelper(threadid_t threadid);
  static void threadFinishHelper(void *arg);

  private:
  /// The frontend command line options
  static FrontendOptions<T>* m_options;
  static FrontendControl<T>* m_control;
  static FELock<T>* new_threadid_lock;
  static thread_data_t* m_thread_data;
  static uint32_t* p_num_threads;  // TODO can this be a reference pointer and work well?

  /// Structure that keeps thread IDs from emulated clone syscalls
  /// that are later used in the frontend thread creation
  static std::deque<uint64_t>* tidptrs;


  //static void threadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v);


};


} // namespace frontend

#include "frontend_threads.tcc"

#endif // __FRONTEND_THREADS_H
