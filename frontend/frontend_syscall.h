#ifndef _FRONTEND_SYSCALL_H_
#define _FRONTEND_SYSCALL_H_

#include "frontend_options.h"
#include "sift_format.h"
#include "frontend_utils.h"
#include "frontend_defs.h"
#include "sift_assert.h"
#include <syscall.h>

namespace frontend
{

/**
 * @class FrontendSyscallModelBase
 *
 * Base class that implements methods related to syscall modeling.
 * Each specific frontend has to specialize a FrontendSyscallModel class that inherits from here.
 * Method emulateSyscallFunc() must be defined in the subclass.
 * T: Frontend type
 */
template <typename T> class FrontendSyscallModelBase
{
  public:
  /// Constructor
  FrontendSyscallModelBase<T>(FrontendOptions<T>* opts, thread_data_t* td, FELock<T> *ntid_lock, std::deque<uint64_t>* tids);

  /// Destructor
  ~FrontendSyscallModelBase<T>();
  
  /// To obtain data accessed by memory instructions in a safe way
  /// This method is passed to Sniper's bridge (trace manager) between frontend and backend.
  static bool handleAccessMemory(void* arg,
                                 Sift::MemoryLockType lock_signal,
                                 Sift::MemoryOpType mem_op,
                                 uint64_t d_addr,
                                 uint8_t* data_buffer,
                                 uint32_t data_size);

  /// Syscall callback: needs to be specialized for each specific frontend
  /// Arguments vary depending on the frontend
  //void emulateSyscallFunc();
  
  protected:
  // Variables
  /// The frontend command line options
  static FrontendOptions<T>* m_options;

  /// Information over the frontend threads
  static thread_data_t* m_thread_data;
  
  /// Lock object used in the creation of new threads
  static FELock<T>* m_new_threadid_lock;
  
  /// Structure that keeps thread IDs from emulated clone syscalls
  /// that are used in the frontend thread creation
  static std::deque<uint64_t>* tidptrs;
  
  /// Memory lock to access a memory location
  // Has to be declared static because it is used in a static method (handleAccessMemory)
  static FELock<T>* m_access_memory_lock;
  
  /// Helper object to manage memory copies
  static FECopy<T> m_copy;
  
  // Methods
  /// Send the thread ID to the backend
  static void setTID(threadid_t threadid);
  
  /// Process syscall number and arguments and send request to the backend
  static void doSyscall(threadid_t threadid, addr_t syscall_number, syscall_args_t& args);

};

template <typename T> class FrontendSyscallModel: public FrontendSyscallModelBase<T>
{
  // To be able to use the constructors with arguments of the superclass - C++'11 syntax
  using FrontendSyscallModelBase<T>::FrontendSyscallModelBase;
  
  public:
    void emulateSyscallFunc();

};


} // namespace frontend

#include "frontend_syscall-inl.h"
#include "frontend_syscall.tcc"

#endif // _FRONTEND_SYSCALL_H_
