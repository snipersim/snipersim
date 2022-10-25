#include <iostream>
#include "sift_writer.h"
#include "sift_assert.h"

namespace frontend
{
  
template <typename T>
ExecFrontend <T>::ExecFrontend(int argc, const char * argv [])
: m_frontend (0),
  m_exec_error (false)
{
  m_options = new FrontendOptions<T>(argc, argv);
  m_exec_error = m_options->parse_cmd_status();

//TODO: should we terminate execution?
  if (m_exec_error) {
    std::cerr << "Error parsing command line arguments" 
      << m_options->cmd_summary() << std::endl;
  // If everything goes fine with the parsing of arguments create the frontend
  } else {
    //m_frontend = new T(m_options->get_theISA());  // For QSim
    m_frontend = new T();
  }
}

template <typename T>
void ExecFrontend <T>::start()
{
  // Initialize thread-related data
  static_assert((sizeof(thread_data_t) % LINE_SIZE_BYTES) == 0, 
    "Error: Thread data should be a multiple of the line size to prevent false sharing");
  
  size_t thread_data_size = MAX_NUM_THREADS * sizeof(*m_frontend->get_thread_data());
  /*if (posix_memalign((void**)&m_thread_data, LINE_SIZE_BYTES, thread_data_size) != 0)
  {
    std::cerr << "Error, posix_memalign() failed" << std::endl;
    exit(1);
  }*/
  m_frontend->allocate_thread_data(thread_data_size);  // frontend specific
  bzero(m_frontend->get_thread_data(), thread_data_size);

  // Create Control, Threads and SyscallModel modules
  m_frontend->init_frontend_modules(m_options);
  
  // when FIFOs are created beforehand, use findMyAppId
  if (m_options->get_emulate_syscalls() || (!m_options->get_use_roi() && !m_options->get_mpi_implicit_roi()))
  {
      if (m_options->get_app_id() < 0)
         m_frontend->get_control()->findMyAppId();
  }
   
  if (m_options->get_fast_forward_target() == 0 && !m_options->get_use_roi() && !m_options->get_mpi_implicit_roi())
  {
    m_frontend->get_control()->set_in_roi(true);
    m_frontend->get_control()->setInstrumentationMode(Sift::ModeDetailed);
    m_frontend->get_control()->openFile(0);
  }
  else if (m_options->get_emulate_syscalls())
  {
    m_frontend->get_control()->openFile(0);
  }
  
  // TODO When attaching with --pid
  
  // Initialize instrumentation callbacks and other frontend features
  handle_frontend_init();
  
  // Set up a function that manages the finalization of execution
  handle_frontend_fini();
  
  // if everything went well, start execution
  // TODO fix this passing around of a big class -- try to do dependency injection
  if(!m_exec_error)
    handle_frontend_start();
} 

// Define static members

template <typename T>
thread_data_t* Frontend<T>::m_thread_data;
template <typename T> 
rombauts::shared_ptr<FrontendSyscallModel<T>> Frontend<T>::m_sysmodel;
template <typename T> 
FrontendThreads<T>* Frontend<T>::m_threads;
template <typename T> 
FrontendCallbacks<T>* Frontend<T>::m_callbacks;
template <typename T> 
FrontendControl<T>* Frontend<T>::m_control;
template <typename T>
FELock<T>* Frontend<T>::new_threadid_lock;
template <typename T>
FrontendOptions<T>* Frontend<T>::m_options;

template <typename T>
void Frontend <T>::init_frontend_modules(FrontendOptions<T>* options)
{
  // Lock for the creation of new threads (used in the Threads and SyscallModel modules)
  new_threadid_lock = new FELock<T>;
  
  // Save options
  m_options = options;

  // Create Syscall module
  m_sysmodel = rombauts::shared_ptr<FrontendSyscallModel<T>>(new FrontendSyscallModel<T>(options, m_thread_data, new_threadid_lock, &(tidptrs)));
  //m_sysmodel = std::make_shared<FrontendSyscallModel<T>>(options, m_thread_data, new_threadid_lock, &(tidptrs));

  // Create Control module
  m_control = new FrontendControl<T>(options, m_thread_data, m_sysmodel);
  
  // Create Callbacks module
  m_callbacks = new FrontendCallbacks<T>(options, m_control, m_thread_data);
  
  // Create Threads module
  m_threads = new FrontendThreads<T>(options, m_control, m_thread_data, &(num_threads), new_threadid_lock, &(tidptrs));
}

template <typename T>
void Frontend <T>::handleMemory(threadid_t threadid, addr_t address)
{
  // We're still called for instructions in the same basic block as ROI end, ignore these
  if (!m_thread_data[threadid].output)
    return;
  
  m_thread_data[threadid].dyn_addresses[m_thread_data[threadid].num_dyn_addresses++] = address;
}

template <typename T>
void Frontend <T>::allocate_thread_data(size_t thread_data_size)
{
  if (posix_memalign((void**)&m_thread_data, LINE_SIZE_BYTES, thread_data_size) != 0)
  {
    std::cerr << "Error, posix_memalign() failed" << std::endl;
    exit(1);
  }
}

} // namespace frontend
