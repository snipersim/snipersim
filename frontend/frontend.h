#ifndef _FRONTEND_H_
#define _FRONTEND_H_

#include "frontend_options.h"
#include "sift_format.h"
#include <deque>
#include <string>
#include "ezOptionParser.hpp"
#include "frontend_threads.h"
#include "frontend_utils.h"
#include "frontend_syscall.h"
#include "frontend_control.h"
#include "frontend_callbacks.h"

namespace frontend
{

/**
 * @class Frontend
 *
 * Base template class for the frontend
 */

template <typename T> class Frontend
{
  public:
  /// Constructors
  Frontend();
  Frontend(FrontendISA theISA);

  /// Destructor
  ~Frontend();

  /// Initialize instrumentation callbacks -- implement in derived frontend class
  void init_instrumentation();

  /// Get the pointer to the thread data structure
  thread_data_t* get_thread_data();

  /// Initialize the modules coupled to Frontend 
  /// (directly coupled: Control, Threads and SyscallModel)
  void init_frontend_modules(FrontendOptions<T>* options);
  
  /// Get a pointer to the frontend control module
  FrontendControl<T>* get_control();

  /// Updates the dynamic count in the frontend of memory arguments 
  static void handleMemory(threadid_t threadid, addr_t address);

  void allocate_thread_data(size_t thread_data_size);

  protected:
  /// Information over the frontend threads -- has to be static to use it in the callbacks
  static thread_data_t* m_thread_data;
  
  /// The frontend control module
  static FrontendControl<T>* m_control;
  
  /// Threads module in the frontend
  static FrontendThreads<T>* m_threads;

  /// Callbacks module in the frontend
  static FrontendCallbacks<T>* m_callbacks;
  
  /// Syscall modeling module
  static rombauts::shared_ptr<FrontendSyscallModel<T>> m_sysmodel;
  
  /// Configuration options
  static FrontendOptions<T>* m_options;

  /// Current number of simulated threads
  uint32_t num_threads;
  
  /// Structure that keeps thread IDs from emulated clone syscalls
  /// that are used in the frontend thread creation
  std::deque<uint64_t> tidptrs;

  /// Lock for the creation of new threads (used in the Threads and SyscallModel modules)
  //shared_ptr<FELock<T>> new_threadid_lock;
  static FELock<T> *new_threadid_lock;

};

/**
 * @class ExecFrontend
 *
 * Runner class to execute the frontend implementations.
 * Initializes arguments and starts execution.
 * T: Frontend type
 */
template <typename T> class ExecFrontend
{
  public:
  /// Constructor: uses the command line arguments to initialize the running environment
  ExecFrontend<T>(int argc, const char* argv[]);

  /// Destructor
  ~ExecFrontend<T>();

  /// Initializes all data and starts the execution of the frontend
  void start();
  
  /// Function wrappers to specialized versions in each specific frontend implementation
  void handle_frontend_init();
  void handle_frontend_start();
  void handle_frontend_fini();
  
  private:
  // Variables
  /// The frontend attached to this execution
  T* m_frontend;

  /// The frontend command line options
  FrontendOptions<T>* m_options;

  /// Execution status to detect errors and terminate the execution
  /// True: an error has occurred
  bool m_exec_error;
  
  // Methods
  /// Change current instrumentation mode
  void setInstrumentationMode(Sift::Mode mode);

};


} // namespace frontend

#include "frontend-inl.h"
#include "frontend.tcc"

#endif // _FRONTEND_H_
