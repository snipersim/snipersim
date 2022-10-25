#include <iostream>
#include <unistd.h>

namespace frontend
{

// Define static members
template <typename T>
thread_data_t* FrontendSyscallModelBase<T>::m_thread_data;
template <typename T>
FELock<T>* FrontendSyscallModelBase<T>::m_access_memory_lock;
template <typename T>
FECopy<T> FrontendSyscallModelBase<T>::m_copy;
template <typename T>
FrontendOptions<T>* FrontendSyscallModelBase<T>::m_options;
template <typename T>
FELock<T>* FrontendSyscallModelBase<T>::m_new_threadid_lock;
template <typename T>
std::deque<uint64_t>* FrontendSyscallModelBase<T>::tidptrs;

template <typename T>
bool FrontendSyscallModelBase <T>::handleAccessMemory(void *arg, Sift::MemoryLockType lock_signal,
  Sift::MemoryOpType mem_op, uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
{
  // Lock memory globally if requested
  // This operation does not occur very frequently, so this should not impact performance
  if (lock_signal == Sift::MemLock)
  {
    if (m_options->get_verbose())
    {
      std::cerr << "Acquire memory lock in handleAccessMemory" << std::endl;
    }
    m_access_memory_lock->acquire_lock(0);
  }

  if (mem_op == Sift::MemRead)
  {
      // The simulator is requesting data from us
      m_copy.copy_from_memory(d_addr, data_buffer, data_size);
  }
  else if (mem_op == Sift::MemWrite)
  {
      // The simulator is requesting that we write data back to memory
      m_copy.copy_to_memory(data_buffer, d_addr, data_size);
  }
  else
  {
    std::cerr << "Error: invalid memory operation type" << std::endl;
    return false;
  }

  if (lock_signal == Sift::MemUnlock)
  {
    if (m_options->get_verbose())
    {
      std::cerr << "Release memory lock in handleAccessMemory" << std::endl;
    }
    m_access_memory_lock->release_lock();
  }
  return true;
}

template <typename T>
void FrontendSyscallModelBase <T>::setTID(threadid_t threadid)
{

   if (m_thread_data[threadid].should_send_threadinfo)
   {
      if (m_options->get_verbose())
      {
         std::cerr << "Set TID for thread: " << threadid << std::endl;
      }
      m_thread_data[threadid].should_send_threadinfo = false;

      Sift::EmuRequest req;
      //Sift::EmuReply res;
      req.setthreadinfo.tid = syscall(__NR_gettid);  // obtains unique thread identifier on Linux
      if (m_options->get_verbose())
      {
         std::cerr << "Setting TID: " << req.setthreadinfo.tid << std::endl;
         if (!m_thread_data[threadid].output)
            std::cerr << "No output yet" << std::endl;
         else
            std::cerr << "Output is open" << std::endl;
      }
      //m_thread_data[threadid].output->Emulate(Sift::EmuTypeSetThreadInfo, req, res);
      if (m_options->get_verbose())
      {
         std::cerr << "TID is set" << std::endl;
      }
   }
}

template <typename T>
void FrontendSyscallModelBase <T>::doSyscall
(threadid_t threadid, addr_t syscall_number, syscall_args_t& args)
{
   if (m_thread_data[threadid].icount_reported > 0)
   {
      //std::cerr << "[SNIPER_FRONTEND] Invoking InstructionCount in doSyscall" << std::endl;
      m_thread_data[threadid].output->InstructionCount(m_thread_data[threadid].icount_reported);
      m_thread_data[threadid].icount_reported = 0;
   }

   // Default: not emulated, override later when needed
   m_thread_data[threadid].last_syscall_emulated = false;

   if (syscall_number == SYS_write && m_thread_data[threadid].output)
   {
      int fd = (int)args[0];
      const char *buf = (const char*)args[1];
      size_t count = (size_t)args[2];

      if (count > 0 && (fd == 1 || fd == 2)){
        //std::cerr << "[SNIPER_FRONTEND] Invoking Output in doSyscall" << std::endl;
        m_thread_data[threadid].output->Output(fd, buf, count);
      
      }
   }

   if (m_options->get_emulate_syscalls() && m_thread_data[threadid].output)
   {
      switch(syscall_number)
      {
         // Handle SYS_clone child tid capture for proper pthread_join emulation.
         // When the CLONE_CHILD_CLEARTID option is enabled, remember its child_tidptr and
         // then when the thread ends, write 0 to the tid mutex and futex_wake it
         case SYS_clone:
         {
            if (args[0] & CLONE_THREAD)
            {
               // Store the thread's tid ptr for later use  -- FIXME!!
               #if defined(TARGET_IA32) || defined(ARM_32) || defined(ARM_64)  // from man clone
                  addr_t tidptr = args[2];
               #elif defined(TARGET_INTEL64) || defined(X86_64)
                  addr_t tidptr = args[3];
               #endif
               if (m_options->get_verbose())
               {
                  std::cerr << "[FRONTEND] Clone thread: going to acquire lock" << std::endl;
               }
               m_new_threadid_lock->acquire_lock(threadid);
               if (m_options->get_verbose())
               {
                  std::cerr << "[FRONTEND] Clone thread: pushing back tidptr" << std::endl;
               }
               tidptrs->push_back(tidptr);
               if (m_options->get_verbose())
               {
                  std::cerr << "[FRONTEND] Clone thread: going to release lock" << std::endl;
               }
               m_new_threadid_lock->release_lock();
               if (m_options->get_verbose())
               {
                  std::cerr << "[FRONTEND] Clone thread: going to create new thread" << std::endl;
               }
               /* New thread */
               m_thread_data[threadid].output->NewThread();
               if (m_options->get_verbose())
               {
                  std::cerr << "[FRONTEND] New thread created" << std::endl;
               }
            }
            else
            {
               /* New process */
               // Nothing to do there, handled in fork()
            }
            break;
         }

         // System calls not emulated (passed through to OS)
         case SYS_read:
         case SYS_write:
         case SYS_wait4:
            m_thread_data[threadid].last_syscall_number = syscall_number;
            m_thread_data[threadid].last_syscall_emulated = false;
            //std::cerr << "[SNIPER_FRONTEND] Invoking Syscall " << syscall_number << " in not emulated option" << std::endl;
            m_thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls emulated (not passed through to OS)
         case SYS_futex:
         case SYS_sched_yield:
         case SYS_sched_setaffinity:
         case SYS_sched_getaffinity:
         case SYS_nanosleep:
            m_thread_data[threadid].last_syscall_number = syscall_number;
            m_thread_data[threadid].last_syscall_emulated = true;
            //std::cerr << "[SNIPER_FRONTEND] Invoking Syscall " << syscall_number << " in emulated option" << std::endl;
            m_thread_data[threadid].last_syscall_returnval = 
            m_thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls sent to Sniper, but also passed through to OS
         case SYS_exit_group:
            //std::cerr << "[SNIPER_FRONTEND] Invoking Syscall " << syscall_number << " in both options" << std::endl;
            m_thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;
         
         default:
            //std::cerr << "[SNIPER_FRONTEND] Doing nothing for Syscall " << syscall_number << std::endl;
            break;

      }
   }
}


} // namespace frontend
