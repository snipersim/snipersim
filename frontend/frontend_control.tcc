#include "sift_format.h"

#include <sys/file.h>
#include <sys/stat.h>

namespace frontend
{
  
template <typename T>
thread_data_t* FrontendControl<T>::m_thread_data;
template <typename T>
FrontendOptions<T>* FrontendControl<T>::m_options;
template <typename T>
shared_ptr<FrontendSyscallModel<T>> FrontendControl<T>::m_sysmodel;

template <typename T>
void FrontendControl <T>::setInstrumentationMode(Sift::Mode mode)
{
  if (m_options->get_current_mode() != mode && mode != Sift::ModeUnknown)
  {
    m_options->set_current_mode(mode);
    switch(mode)
    {
      case Sift::ModeIcount:
        this->set_any_thread_in_detail(false);
        break;
      case Sift::ModeMemory:
      case Sift::ModeDetailed:
        this->set_any_thread_in_detail(true);
        break;
      case Sift::ModeStop:
        for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
        {
          if (m_thread_data[i].output)
            closeFile(i);
        }
        exit(0);
      case Sift::ModeUnknown:
        assert(false);
    }
    removeInstrumentation();
  }
}

template <typename T>
void FrontendControl <T>::openFile(threadid_t threadid)
{
  if (m_thread_data[threadid].output)
  {
    closeFile(threadid);
    ++m_thread_data[threadid].blocknum;
  }

  if (m_thread_data[threadid].thread_num != 0)
  {
    sift_assert(m_options->get_response_files() != 0);
  }

  char filename[1024] = {0};
  char response_filename[1024] = {0};
  char remote_filename[1024] = {0};
  char remote_response_filename[1024] = {0};
  if (m_options->get_response_files() == false)
  {
    if (m_options->get_blocksize()) {
      sprintf(filename, "%s.%" PRIu64 ".sift", m_options->get_output_file().c_str(), m_thread_data[threadid].blocknum);
      sprintf(remote_filename, "/home/cecilia/work/sniper/internal_mod/sniper/ssh_dir/run_benchmarks.%" PRIu64 ".sift", m_thread_data[threadid].blocknum);
    } else {
      sprintf(filename, "%s.sift", m_options->get_output_file().c_str());
      sprintf(remote_filename, "/home/cecilia/work/sniper/internal_mod/sniper/ssh_dir/run_benchmarks.sift");
    }
  }
  else
  {
    if (m_options->get_blocksize()) {
      sprintf(filename, "%s.%" PRIu64 ".app%" PRId32 ".th%" PRIu64 ".sift", m_options->get_output_file().c_str(), m_thread_data[threadid].blocknum, m_options->get_app_id(), m_thread_data[threadid].thread_num);
      sprintf(remote_filename, "/home/cecilia/work/sniper/internal_mod/sniper/ssh_dir/run_benchmarks.%" PRIu64 ".app%" PRId32 ".th%" PRIu64 ".sift", m_thread_data[threadid].blocknum, m_options->get_app_id(), m_thread_data[threadid].thread_num);
    } else {
      sprintf(filename, "%s.app%" PRId32 ".th%" PRIu64 ".sift", m_options->get_output_file().c_str(), m_options->get_app_id(), m_thread_data[threadid].thread_num);
      sprintf(remote_filename, "/home/cecilia/work/sniper/internal_mod/sniper/ssh_dir/run_benchmarks.app%" PRId32 ".th%" PRIu64 ".sift", m_options->get_app_id(), m_thread_data[threadid].thread_num);
    }
  }

  if (m_options->get_verbose())
    std::cerr << "[SNIPER_FRONTEND:" << m_options->get_app_id() << ":" << m_thread_data[threadid].thread_num << "] Output = [" << filename << "]" << std::endl;

  if (m_options->get_response_files())
  {
    sprintf(response_filename, "%s_response.app%" PRId32 ".th%" PRIu64 ".sift", m_options->get_output_file().c_str(), m_options->get_app_id(), m_thread_data[threadid].thread_num);
    sprintf(remote_response_filename, "/home/cecilia/work/sniper/internal_mod/sniper/ssh_dir/run_benchmarks_response.app%" PRId32 ".th%" PRIu64 ".sift", m_options->get_app_id(), m_thread_data[threadid].thread_num);
    if (m_options->get_verbose())
      std::cerr << "[SNIPER_FRONTEND:" << m_options->get_app_id() << ":" << m_thread_data[threadid].thread_num << "] Response = [" << response_filename << "]" << std::endl;
  }

  // Open the file for writing
  // TODO adapt this to a more cross platform friendly version
  try {
    #if defined(TARGET_IA32) || defined(ARM_32)
      const bool arch32 = true;
    #else
      const bool arch32 = false;
    #endif
    m_thread_data[threadid].output = new Sift::Writer (filename, this->getCode, 
                                                       m_options->get_response_files() ? false : true,
                                                       response_filename, threadid, arch32, false, 
                                                       m_options->get_send_physical_address());
    // Frontend and backend communicate over network: open socat pipes
    if (m_options->get_ssh())
    {
      std::cerr << "SOCAT at work" << std::endl;
      // Send trace, invoking socat at frontend machine (as server)
      // Receive trace, invoking socat at backend machine over ssh (as client)
      std::string command = "ssh cecilia@172.18.20.127 socat -u TCP-LISTEN:8888,reuseaddr OPEN:";
      command += (const char*)remote_filename;
      command += ",creat";
      FILE *in1, *in2, *in3, *in4;
      if(!(in1 = popen(command.c_str(), "r"))){
        std::cerr << "Error executing command: " << command << std::endl;
      } else {
        std::cerr << "Command: " << command << std::endl;
      }
      command = "socat -u FILE:";
      command += (const char*)filename;
      command += ",ignoreeof TCP:172.18.20.127:8888,retry";
      if(!(in2 = popen(command.c_str(), "r"))){
        std::cerr << "Error executing command: " << command << std::endl;
      } else {
        std::cerr << "Command: " << command << std::endl;
      }
        std::cerr << "File closed for command: " << command << std::endl;

      // Open also response files pipes?
      if (m_options->get_response_files())
      {
        // Send response, invoking socat at backend machine over ssh (as server)
        command = "ssh cecilia@172.18.20.127 socat -u FILE:";
        command += (const char*)remote_response_filename;
        command += ",ignoreeof TCP-LISTEN:9999,reuseaddr";
        if(!(in3 = popen(command.c_str(), "r"))){
          std::cerr << "Error executing command: " << command << std::endl;
      } else {
        std::cerr << "Command: " << command << std::endl;
      }
        // Receive response, invoking socat at frontend machine (as client)
        command = "socat -u TCP:172.18.20.127:9999,retry OPEN:";
        command += (const char*)response_filename;
        command += ",append";
        if(!(in4 = popen(command.c_str(), "r"))){
          std::cerr << "Error executing command: " << command << std::endl;
      } else {
        std::cerr << "Command: " << command << std::endl;
      }
      }
    }
  } catch (...) {
    std::cerr << "[SNIPER_FRONTEND:" << m_options->get_app_id() << ":" << m_thread_data[threadid].thread_num << "] Error: Unable to open the output file " << filename << std::endl;
    exit(1);
  }

  m_thread_data[threadid].output->setHandleAccessMemoryFunc(m_sysmodel->handleAccessMemory, reinterpret_cast<void*>(threadid));

}


template <typename T>
void FrontendControl <T>::closeFile(threadid_t threadid)
{
  if (m_options->get_verbose())
  {
    std::cerr << "[SNIPER_FRONTEND:" << m_options->get_app_id() << ":" << m_thread_data[threadid].thread_num << "] Recorded " << m_thread_data[threadid].icount_detailed;
    if (m_thread_data[threadid].icount > m_thread_data[threadid].icount_detailed)
      std::cerr << " (out of " << m_thread_data[threadid].icount << ")";
    std::cerr << " instructions" << std::endl;
  }

   Sift::Writer *output = m_thread_data[threadid].output;
   m_thread_data[threadid].output = NULL;
   // Thread will stop writing to output from this point on
           std::cerr << "[SNIPER_FRONTEND] Invoking End" << std::endl;

   output->End();
   delete output;

   if (m_options->get_blocksize())
   {
      if (m_thread_data[threadid].bbv_count)
      {
         m_thread_data[threadid].bbv->count(m_thread_data[threadid].bbv_base, m_thread_data[threadid].bbv_count);
         m_thread_data[threadid].bbv_base = 0; // Next instruction starts a new basic block
         m_thread_data[threadid].bbv_count = 0;
      }

      char filename[1024];
      sprintf(filename, "%s.%" PRIu64 ".bbv", m_options->get_output_file().c_str(), m_thread_data[threadid].blocknum);

      FILE *fp = fopen(filename, "w");
      fprintf(fp, "%" PRIu64 "\n", m_thread_data[threadid].bbv->getInstructionCount());
      for(int i = 0; i < Bbv::NUM_BBV; ++i)
        fprintf(fp, "%" PRIu64 "\n", m_thread_data[threadid].bbv->getDimension(i) / m_thread_data[threadid].bbv->getInstructionCount());
      fclose(fp);

      m_thread_data[threadid].bbv->clear();
   }
}

template <typename T>
void FrontendControl <T>::Fini(int32_t code, void *v)
{
  for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
  {
    if (m_thread_data[i].output)
    {
      closeFile(i);
    }
  }
  size_t thread_data_size = MAX_NUM_THREADS * sizeof(*m_thread_data);
  /*dr_custom_free(NULL, (dr_alloc_flags_t) 0, m_thread_data, thread_data_size);*/
  FrontendControl <T>::free_thread_data(thread_data_size);
}

template <typename T>
void FrontendControl <T>::Fini()
{
  Fini(0, 0);
}

template <typename T>
void FrontendControl <T>::getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
  std::cerr << "[FRONTEND] Executing getCode: dst: "<< &dst << ", src: " << (void*)src << ", size:" << size << std::endl;
  std::memcpy( dst, src, size );  // TODO substitute by template
  std::cerr << "[FRONTEND] getCode executed" << std::endl;
}

template <typename T>
void FrontendControl <T>::beginROI(threadid_t threadid)
{
   if (m_options->get_app_id() < 0)
      findMyAppId();

   if (in_roi)
   {
      std::cerr << "[SIFT_RECORDER:" << m_options->get_app_id() << "] Error: ROI_START seen, but we have already started." << std::endl;
   }
   else
   {
      if (m_options->get_verbose())
         std::cerr << "[SIFT_RECORDER:" << m_options->get_app_id() << "] ROI Begin" << std::endl;
   }

   in_roi = true;
   setInstrumentationMode(Sift::ModeDetailed);

   if (m_options->get_emulate_syscalls())
   {
      if (m_thread_data[threadid].icount_reported > 0)
      {
         m_thread_data[threadid].output->InstructionCount(m_thread_data[threadid].icount_reported);
         m_thread_data[threadid].icount_reported = 0;
      }
   }
   else
   {
      for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
      {
         if (m_thread_data[i].running && !m_thread_data[i].output)
            openFile(i);
      }
   }
}

template <typename T>
void FrontendControl <T>::endROI(threadid_t threadid)
{
   if (m_options->get_emulate_syscalls())
   {
        // In simulations with MPI, generate the app crash
        // with the first MPI_Finalize
        if (!m_options->get_mpi_implicit_roi())
        {
           // Send SYS_exit_group to the simulator to end the application
           syscall_args_t args = {0};
           args[0] = 0; // Assume success
           m_thread_data[threadid].output->Syscall(SYS_exit_group, (char*)args, sizeof(args));
           m_thread_data[threadid].output->End();
       }
    }

   // Delete our .appid file
   char filename[1024] = {0};
   sprintf(filename, "%s.app%" PRId32 ".appid", m_options->get_output_file().c_str(), m_options->get_app_id());
   unlink(filename);

   if (m_options->get_verbose())
      std::cerr << "[SIFT_RECORDER:" << m_options->get_app_id() << "] ROI End" << std::endl;

   // Stop threads from sending any more data while we close the SIFT pipes
   setInstrumentationMode(Sift::ModeIcount);
   
   in_roi = false;

   if (!m_options->get_response_files())
   {
      for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
      {
         if (m_thread_data[i].running && m_thread_data[i].output)
            closeFile(i);
      }
   }
}

template <typename T>
void FrontendControl <T>::findMyAppId()
{
   // FIFOs for thread 0 of each application have been created beforehand
   // Protocol to figure out our application id is to lock a .app file
   // Try to lock from app_id 0 onwards, claiming the app_id if the lock succeeds,
   // and fail when there are no more files
   for(uint64_t id = 0; id < 1000; ++id)
   {
      char filename[1024] = {0};

      // First check whether this many appids are supported by stat()ing the request FIFO for thread 0
      struct stat sts;
      sprintf(filename, "%s.app%" PRIu64 ".th%" PRIu64 ".sift", m_options->get_output_file().c_str(), id, (uint64_t)0);
      if (stat(filename, &sts) != 0)
      {
         break;
      }

      // Atomically create .appid file
      sprintf(filename, "%s.app%" PRIu64 ".appid", m_options->get_output_file().c_str(), id);
      int fd = open(filename, O_CREAT | O_EXCL, 0600);
      if (fd != -1)
      {
         // Success: use this app_id
         m_options->set_app_id(id);
         std::cerr << "[SIFT_RECORDER:" << m_options->get_app_id() << "] Application started" << std::endl;
         return;
      }
      // Could not create, probably someone else raced us to it. Try next app_id
   }
   std::cerr << "[SIFT_RECORDER] Cannot find free application id, too many processes!" << std::endl;
   exit(1);
}

} // namespace frontend
