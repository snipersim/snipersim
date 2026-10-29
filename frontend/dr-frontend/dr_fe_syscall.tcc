namespace frontend
{

std::unordered_map<int, threadid_t>* FrontendSyscallModel<DRFrontend>::map_threadids;

// template specialization of the Syscall callback
bool FrontendSyscallModel<DRFrontend>::event_pre_syscall(void *drcontext, int syscall_number)
{
  // 1: Send a thread ID to the backend if not done yet :
  int threadid = DRFrontend::fe_tid(drcontext);
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

  // The instructions before the call first: the guest's clocks count them,
  // and the simulator sees them before the call
  DRFrontend::process_instructions_buffer(drcontext);
  const bool emulate = m_options->get_emulate_syscalls();
  if (emulate && m_thread_data[threadid].output && m_thread_data[threadid].icount_reported > 0)
  {
    m_thread_data[threadid].output->InstructionCount(m_thread_data[threadid].icount_reported);
    m_thread_data[threadid].icount_reported = 0;
  }

  // 3: The guest's time (dr_fe_time.cc): the clocks without a simulator,
  // the timers, the timed waits the simulator does not model -- answered
  // there, the call skipped
  {
    uint64_t a[MAX_SYSCALL_ARGS], ret;
    for (int i = 0; i < MAX_SYSCALL_ARGS; i++)
      a[i] = args[i];
    if (drtime::syscall(threadid, syscall_number, a, &ret))
    {
      m_thread_data[threadid].last_syscall_emulated = false;
      dr_syscall_set_result(drcontext, (reg_t)ret);
      return false;
    }
  }

  // 4: Process the syscall and send to the backend to check if it has to be emulated
  if (emulate)
    doSyscall(threadid, syscall_number, args);
  else
    m_thread_data[threadid].last_syscall_emulated = false;

  // 5: Post-process
  if (!m_thread_data[threadid].last_syscall_emulated)  // Not emulated syscall, execute normally
  {
    // The thread's last system call: end its trace now, while DynamoRIO
    // holds no locks, rather than in the thread-exit event (see there)
    if (syscall_number == SYS_exit)
      DRFrontend::finish_thread(drcontext);
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

void FrontendSyscallModel<DRFrontend>::event_post_syscall(void *drcontext, int syscall_number)
{
  // A call the thread may have waited in returned (the one-IPC model)
  drtime::syscall_returned(DRFrontend::fe_tid(drcontext), (int64_t)dr_syscall_get_result(drcontext));
}

bool FrontendSyscallModel<DRFrontend>::event_filter_syscall(void *drcontext, int sysnum)
{
    return true; // intercept everything
}

void FrontendSyscallModel<DRFrontend>::initSyscallModeling()
{
  dr_register_filter_syscall_event(event_filter_syscall);
  drmgr_register_pre_syscall_event(event_pre_syscall);
  drmgr_register_post_syscall_event(event_post_syscall);
}

} // end namespace frontend