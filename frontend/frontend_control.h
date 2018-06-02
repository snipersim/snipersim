#ifndef _FRONTEND_CONTROL_H_
#define _FRONTEND_CONTROL_H_

#include "frontend_options.h"
#include "sift_format.h"
#include "frontend_threads.h"
#include "frontend_utils.h"
#include "frontend_syscall.h"

namespace frontend
{
  
/**
 * @class FrontendControl
 *
 * Class that implements methods to control the framework's behaviour.
 * T: Frontend type
 */
template <typename T> class FrontendControl
{
  public:
  /// Constructor
  FrontendControl<T>(FrontendOptions<T>* opts, thread_data_t* td, shared_ptr<FrontendSyscallModel<T>> sysmodel);

  /// Destructor
  ~FrontendControl<T>();
  
  /// Closes all the communication pipes between frontend and Sniper backend threads
  static void Fini(int32_t code, void *v);
  /// Another signature of the same function to let DynamoRIO register it
  static void Fini();

  /// Open the pipe files used to communicate with Sniper
  void openFile(threadid_t threadid);

  /// Close the pipe files used to communicate with Sniper
  static void closeFile(threadid_t threadid);

  /// Change current instrumentation mode
  void setInstrumentationMode(Sift::Mode mode);
  
  /// To obtain the executed instructions from the frontend tool to pass to Sniper's backend
  /// This method is passed to Sniper's bridge (trace manager) between frontend and backend.
  static void getCode(uint8_t* dst, const uint8_t* src, uint32_t size);
  
  /// Begin or end a Region of Interest and notify the backend
  void beginROI(threadid_t threadid);
  void endROI(threadid_t threadid);
  
  /// Find the ID of an application whose pipes have already been created
  void findMyAppId();
  
  /// Change the value to in_roi, to specify if we enter or leave a Region of Interest. 
  void set_in_roi(bool ir);
  
  /// Get the in_roi value.
  bool get_in_roi();
  
  /// Get and set the value of member any_thread_in_detail
  bool get_any_thread_in_detail();
  void set_any_thread_in_detail(bool in_detail);
  
  /// Get the value of the stop address specified by the user
  uint64_t get_stop_address();

  private:
  // Variables
  /// The frontend command line options
  static FrontendOptions<T>* m_options;

  /// Information over the frontend threads
  static thread_data_t* m_thread_data;
  
  /// Syscall modeling module
  static shared_ptr<FrontendSyscallModel<T>> m_sysmodel;

  // Methods
  /// Eliminate instrumentation from the running application -- might be optionally specialized
  void removeInstrumentation();
  
  static void free_thread_data(size_t thread_data_size);

  /// Execution currently in Region Of Interest
  bool in_roi;

  /// To keep track of detailed simulation in any of the threads
  bool any_thread_in_detail;
};

} // namespace frontend

#include "frontend_control-inl.h"
#include "frontend_control.tcc"

#endif // _FRONTEND_H_
