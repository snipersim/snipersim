#ifndef _FRONTEND_CALLBACKS_H_
#define _FRONTEND_CALLBACKS_H_

#include "frontend_options.h"
#include "sift_format.h"
#include "frontend_threads.h"
#include "frontend_utils.h"
#include "frontend_control.h"

namespace frontend
{
  
/**
 * @class FrontendCallbacks
 *
 * Class that implements support functions that are called during an application execution to send
 * the instruction sequence and other information to Sniper's backend.
 * T: Frontend type
 */
template <typename T> class FrontendCallbacks
{
  public:
  /// Constructor
  FrontendCallbacks<T>(FrontendOptions<T>* opts, FrontendControl<T>* cntrl, thread_data_t* td);

  /// Destructor
  ~FrontendCallbacks<T>();
  
  
  /// Method to update the instruction count in the frontend and the backend.
  /// Evaluates input options and current conditions to eventually change the simulation mode.
  static void countInsns(threadid_t threadid, int32_t count);
  
  /// Method to send an instruction to Sniper's backend; to be executed within an instruction callb.
  static void sendInstruction(threadid_t threadid, addr_t addr, uint32_t size, uint32_t num_addresses, bool is_branch, 
                              bool taken, bool is_predicate, bool executing, bool isbefore, bool ispause);
  
  /// Code specific to the running frontend implementation to be executed inside sendInstruction
  static void __sendInstructionSpecialized(threadid_t threadid, uint32_t num_addresses, bool isbefore);
  
  /// Method to tell the backend that the upcoming instruction(s) has/ve a new ISA mode 
  static void changeISA(threadid_t threadid, int new_isa);
  
  /// Method to send a magic instruction (command) to the backend
  static addr_t handleMagic(threadid_t threadid, addr_t reg_0, addr_t reg_1, addr_t reg_2);
  
  private:
  // Variables
  /// The frontend command line options
  static FrontendOptions<T>* m_options;
  
  /// Frontend module to control the simulation (ie. sets the simulation mode)
  static FrontendControl<T>* m_control;
  
  /// Information over the frontend threads
  static thread_data_t* m_thread_data;
  
};

} // namespace frontend

#include "frontend_callbacks-inl.h"
#include "frontend_callbacks.tcc"

#endif // _FRONTEND_CALLBACKS_H_