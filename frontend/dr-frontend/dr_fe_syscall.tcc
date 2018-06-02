namespace frontend
{

std::unordered_map<int, threadid_t>* FrontendSyscallModel<DRFrontend>::map_threadids;

// template specialization of the Syscall callback
bool FrontendSyscallModel<DRFrontend>::event_pre_syscall(void *drcontext, int syscall_number)
{
  // 1: Send a thread ID to the backend if not done yet :
  int threadid = (*map_threadids)[dr_get_thread_id(drcontext)]; 
  setTID(threadid);

  // 2: Collect frontend-dependent syscall args 
  IF_ARM_ELSE
  (
    // ARM syscalls in Thumb mode
    sift_assert(syscall_number < 378 || 
    // ARM syscalls in ARM mode
                (syscall_number > 9437184 && syscall_number < 9437561) ||
    // set_tls
                syscall_number == 983045),
    // Not ARM
    sift_assert(syscall_number < MAX_NUM_SYSCALLS)
  );
  syscall_args_t args;
  for (int i = 0; i < MAX_SYSCALL_ARGS; i++)
    args[i] = dr_syscall_get_param(drcontext, i);
  
  // 3: Process the syscall and send to the backend to check if it has to be emulated
  doSyscall(threadid, syscall_number, args);

  // 4: Post-process
  if (!m_thread_data[threadid].last_syscall_emulated)  // Not emulated syscall, execute normally
  {
    return true;  // execute this syscall
  }
  else  // Emulated syscall, skip execution, setting return value from Sniper's emulation
  {
    dr_syscall_set_result(drcontext, m_thread_data[threadid].last_syscall_returnval);
    m_thread_data[threadid].last_syscall_emulated = false;
    return false;  // to skip this syscall
  }
   //return true;
}

bool FrontendSyscallModel<DRFrontend>::event_filter_syscall(void *drcontext, int sysnum)
{
    return true; // intercept everything
}

void FrontendSyscallModel<DRFrontend>::initSyscallModeling()
{
  dr_register_filter_syscall_event(event_filter_syscall);
  drmgr_register_pre_syscall_event(event_pre_syscall);
}

} // end namespace frontend