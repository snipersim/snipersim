#include "dr_frontend.h"

#include "drmgr.h"
#include "drutil.h"
#include "drreg.h"

#include "dr_fe_options.h"

#include <errno.h>
#include <string.h>

/* The magic instruction (sim_api.h) and the registers it uses: x86
 * "xchg %bx, %bx" with the command and answer in RAX and the arguments in RBX
 * and RCX; AArch64 "bfm x0, x0, #0, #0" with the answer in X0, the command in
 * X1 and the arguments in X2 and X3. */
#ifdef AARCH64
static const reg_id_t magic_regs[] = { DR_REG_X0, DR_REG_X1, DR_REG_X2, DR_REG_X3 };
#else
static const reg_id_t magic_regs[] = { DR_REG_RAX, DR_REG_RBX, DR_REG_RCX };
#endif

static bool is_magic_instr(instr_t *instr)
{
  IF_X86
  (
    return instr_get_opcode(instr) == OP_xchg &&
           instr_num_srcs(instr) == 2 && instr_num_dsts(instr) == 2 &&
           opnd_is_reg(instr_get_src(instr, 0)) &&
           opnd_get_reg(instr_get_src(instr, 0)) == DR_REG_BX &&
           opnd_is_reg(instr_get_dst(instr, 0)) &&
           opnd_get_reg(instr_get_dst(instr, 0)) == DR_REG_BX;
  )
  IF_AARCH64
  (
    return instr_get_opcode(instr) == OP_bfm &&
           instr_reg_in_src(instr, DR_REG_X0) &&
           instr_reg_in_dst(instr, DR_REG_X0) &&
           instr_num_srcs(instr) == 4 &&
           opnd_get_immed_int(instr_get_src(instr, 2)) == 0 &&
           opnd_get_immed_int(instr_get_src(instr, 3)) == 0;
  )
  return false;
}

/* The scratch registers the tracing instrumentation may borrow in a block
 * holding a magic instruction: any but the magic registers.  drreg lends a
 * register it finds dead without saving the application's value, and its
 * liveness cannot see the magic instruction's implicit use -- in
 * "mov $14, %rax; mov $name, %rbx; xchg %bx, %bx; mov $1, %rax" RAX is
 * written next and never read, so drreg lent it to the tracer and the magic
 * clean call read the tracer's buffer pointer (the simulator saw command
 * 0x400000).  Under DynamoRIO every command with an argument or an effect was
 * lost that way.  Other blocks keep the full choice. */
static drvector_t magic_free_regs;

static void init_magic_free_regs()
{
  drreg_init_and_fill_vector(&magic_free_regs, true);
  for (reg_id_t r : magic_regs)
    drreg_set_vector_entry(&magic_free_regs, r, false);
}

static drvector_t *scratch_allowed(instrlist_t *bb)
{
  for (instr_t *i = instrlist_first_app(bb); i != NULL; i = instr_get_next_app(i))
    if (is_magic_instr(i))
      return &magic_free_regs;
  return NULL;
}

#include "dr_fe_copy.h"
#include "dr_fe_time.h"
#include "dr_fe_syscall.h"
#include "dr_fe_lock.h"
#include "dr_fe_control.h"
#include "dr_fe_threads.h"
#include "sift_writer.h"

// Static fields
std::unordered_map<int, threadid_t> DRFrontend::map_threadids;
void *DRFrontend::thread_lock = NULL;

threadid_t DRFrontend::fe_tid(void *drcontext)
{
  return ((per_thread_t *) drmgr_get_tls_field(drcontext, tls_idx))->fe_tid;
}
threadid_t DRFrontend::next_threadid = 0;
app_pc DRFrontend::exe_start;
int DRFrontend::tls_idx;
reg_id_t DRFrontend::tls_seg;
uint DRFrontend::tls_offs;
app_pc DRFrontend::last_opnd;
int DRFrontend::last_mode = -1;

/* DynamoRIO's dr_isa_mode_t and Sniper's dl_isa number the ISAs differently:
   DR_ISA_ARM_A32 is 2, which as a dl_isa is DL_ISA_RISCV, and DR_ISA_ARM_THUMB
   3, which is DL_ISA_ARM32.  A mode is mapped, never passed through; -1 for
   one Sniper has no ISA for (a DynamoRIO newer than this list). */
static inline int sniperIsaOf(int dr_mode)
{
  switch (dr_mode)
  {
    case DR_ISA_IA32:        return dl::DL_ISA_IA32;
    case DR_ISA_AMD64:       return dl::DL_ISA_X86_64;
    case DR_ISA_ARM_A32:     return dl::DL_ISA_ARM32;
    case DR_ISA_ARM_THUMB:   return dl::DL_ISA_THUMB;
    case DR_ISA_ARM_A64:     return dl::DL_ISA_AARCH64;
    case DR_ISA_RV64IMAFDC:  return dl::DL_ISA_RISCV;
    default:                 return -1;
  }
}

// -----------------------------------------------------------
// Specialization of functions declared in frontend.h classes
// -----------------------------------------------------------

namespace frontend
{
  
template <> void ExecFrontend<DRFrontend>::handle_frontend_init()
{
  m_frontend->init();
}

template <> void ExecFrontend<DRFrontend>::handle_frontend_start()
{
  m_frontend->start();
}

template <> void ExecFrontend<DRFrontend>::handle_frontend_fini()
{
  dr_register_exit_event(m_frontend->get_control()->Fini);
  dr_register_exit_event(m_frontend->event_exit);
}

} // END namespace frontend

// ----------------------------------------------------
// Specialization of methods from the Frontend template
// ----------------------------------------------------

void DRFrontend::allocate_thread_data(size_t thread_data_size)
{
  //std::cerr << "Inside allocate" << std::endl;
  m_thread_data = (thread_data_t*) dr_custom_alloc(NULL, (dr_alloc_flags_t) 0, thread_data_size, 0, NULL);
}

// Frontend initialization, called from the specialization of ExecFrontend::handle_frontend_init().
void DRFrontend::init()
{
  //std::cerr << "Init specific frontend" << std::endl;

  this->num_threads = 0;
  // Initialize options for scratch registers - We need 2 reg slots beyond drreg's eflags slots => 3 slots 
  drreg_options_t ops = {};
  ops.struct_size = sizeof(ops);
  ops.num_spill_slots = 3;
  ops.conservative = false;  

  if(!drmgr_init() || drreg_init(&ops) != DRREG_SUCCESS)
    DR_ASSERT(false);
  init_magic_free_regs();
    
  dr_set_client_name("Sniper's frontend based on DynamoRIO", "http://snipersim.org");

  // Code rewritten in place is tracked unless -smc 0 (Sift::CodeTracker)
  Sift::CodeTracker::enable(m_options->get_smc_track());
  
  // Get main module address
  module_data_t *exe = dr_get_main_module();
  if (exe != NULL)
    this->exe_start = exe->start;
  dr_free_module_data(exe);

  // Init syscall emulation if response files
  if (DREmulateSyscalls.get_value() && !DRUseResponseFiles.get_value())
  {
    std::cerr << "Error, Response files are required when using syscall emulation." << std::endl;
    exit(1);
  }

  // The guest's time (dr_fe_time.cc): its system calls are looked at with
  // or without a simulator -- without one, the one-IPC model answers the
  // clocks and times the waits and the timers
  // Where it is not kept (another ISA than x86-64, -refuse_time), the
  // guest's first use of time ends the run, with the reason
  drtime::init(m_thread_data, DREmulateSyscalls.get_value() != 0, DRTimeStart.get_value(),
               DRRefuseTime.get_value(), DROutputFile.get_value().c_str(), DRSiftAppId.get_value());
  if (DREmulateSyscalls.get_value() || drtime::supported() || drtime::refusing())
  {
    m_sysmodel->set_map_threads(&map_threadids);
    m_sysmodel->initSyscallModeling();
  }
  if (drtime::supported() || drtime::refusing())
  {
    // The vDSO's clocks (glibc's clock_gettime, gettimeofday and time make
    // no system call) and rdtsc (AArch64: the counter's mrs)
    if (!drmgr_register_module_load_event(event_module_load) ||
        !drmgr_register_bb_app2app_event(event_app2app, NULL) ||
        !drmgr_register_signal_event(event_signal))
      DR_ASSERT(false);
    // the vDSO is mapped before the client starts
    dr_module_iterator_t *mi = dr_module_iterator_start();
    while (dr_module_iterator_hasnext(mi))
    {
      module_data_t *mod = dr_module_iterator_next(mi);
      event_module_load(NULL, mod, true);
      dr_free_module_data(mod);
    }
    dr_module_iterator_stop(mi);
  }

  // Init callbacks
  if (!drmgr_register_thread_init_event(event_thread_init) ||
      !drmgr_register_thread_exit_event(event_thread_exit) ||
      !drmgr_register_bb_instrumentation_event( event_bb_analysis,  // counts instructions
                                                event_app_instruction,  // sends instructions to countInst function
                                                NULL))
  {
    DR_ASSERT(false);
  }
  
  // Reserves a thread-local storage (tls) slot for every thread, returning the index of the slot
  tls_idx = drmgr_register_tls_field();
  thread_lock = dr_mutex_create();
  DR_ASSERT(tls_idx != -1);
  // The TLS field provided by DR cannot be directly accessed from the code cache.
  // For better performance, we allocate raw TLS so that we can directly access and update it with a single instruction.
  if (!dr_raw_tls_calloc(&tls_seg, &tls_offs, FRONTEND_TLS_COUNT, 0))
    DR_ASSERT(false);
     
}

// Explicitely starting a program is necessary with other tools, but not with DR
void DRFrontend::start()
{
}

///////////////////////////////////////////////////////////////
////////////////   DynamoRIO callbacks       //////////////////
///////////////////////////////////////////////////////////////

void DRFrontend::event_thread_init(void *drcontext)
{        
  // create an instance of the data structure that saves the instruction information for this thread
  per_thread_t *data = (per_thread_t *) dr_thread_alloc(drcontext, sizeof(per_thread_t));
  DR_ASSERT(data != NULL);
  // store it in the slot of the thread local storage (TLS) provided in the drcontext and initialize its fields
  drmgr_set_tls_field(drcontext, tls_idx, data);

  // Keep seg_base in a per-thread data structure so we can get the TLS slot and find where the pointer points to in the buffer.
  data->seg_base = (byte*) dr_get_dr_segment_base(tls_seg);
  data->inst_buf = (instruction_t*)dr_raw_mem_alloc(MEM_BUF_SIZE, DR_MEMPROT_READ | DR_MEMPROT_WRITE, NULL);
  DR_ASSERT(data->seg_base != NULL && data->inst_buf != NULL);
  // put inst_buf to TLS as starting buf_ptr 
  BUF_PTR(data->seg_base) = data->inst_buf;

  // Assign this thread's front-end id.  DynamoRIO runs thread-init events
  // concurrently, in the new threads: unlocked, two threads could take the
  // same id -- and share one stream, the second open closing the first's --
  // while the shared table was being rehashed under other threads' lookups.
  int threadid_dr = dr_get_thread_id(drcontext);
  dr_mutex_lock(thread_lock);
  threadid_t my_threadid = next_threadid++;
  map_threadids[threadid_dr] = my_threadid;
  dr_mutex_unlock(thread_lock);
  data->fe_tid = my_threadid;
  data->taken = new std::unordered_map<ptr_uint_t, ptr_uint_t>();
  data->finished = false;

  // Call the generic frontend callback for a thread start, with the stack
  // pointer the thread starts with: clone() set it, and the generic layer
  // pairs the thread with its own pthread_join word by it.  DynamoRIO hands
  // out the application context in the init event of every thread but the
  // first, which has no join word anyway.
  uint64_t start_sp = 0;
  dr_mcontext_t mc;
  mc.size = sizeof(mc);
  mc.flags = DR_MC_CONTROL;
  if (dr_get_mcontext(drcontext, &mc))
    start_sp = (uint64_t)mc.xsp;
  m_threads->threadStart(my_threadid, start_sp);
  // Its clock starts at its parent's time
  drtime::thread_start(my_threadid, start_sp);

  if (m_options->get_verbose())
  {
    std::cerr << "[Thread init end] Threadid: " << my_threadid << std::endl;
  }
}

void DRFrontend::finish_thread(void *drcontext)
{
  per_thread_t *data = (per_thread_t *) drmgr_get_tls_field(drcontext, tls_idx);
  if (data->finished)
    return;
  data->finished = true;
  process_instructions_buffer(drcontext); // dump remaining contents
  drtime::thread_exit(fe_tid(drcontext));
  m_threads->threadFinish(fe_tid(drcontext), 0);  // TODO are flags here relevant?
}

void DRFrontend::event_thread_exit(void *drcontext)
{
  /* Normally done already, at the thread's exit system call.  Doing it here
     blocks while DynamoRIO holds thread_initexit_lock -- it calls this event
     with that lock held -- and any other thread that needs the lock (to
     start or end, or to flush code) then stops before its next trace
     record.  The simulator's barrier waits for that thread's stream, so it
     never answers this thread's FUTEX_WAKE or End, and nothing moves again
     (test/dr-threads hung in about one run in thirty).  Here only for
     threads that did not get there, such as those ended by exit_group. */
  finish_thread(drcontext);

  threadid_t threadid_fe = fe_tid(drcontext);
  if (m_options->get_verbose())
  {
    std::cerr << "[Thread exit] Threadid: " << threadid_fe << std::endl;
  }
  if (m_options->get_verbose())
  {
    if(dr_using_all_private_caches())
      std::cerr << "[Thread exit] Using private caches."  << std::endl;
  }
  
  // free allocated memory for tls 
  per_thread_t *data = (per_thread_t *) drmgr_get_tls_field(drcontext, tls_idx);
  dr_raw_mem_free(data->inst_buf, MEM_BUF_SIZE);
  delete data->taken;
  dr_thread_free(drcontext, data, sizeof(per_thread_t));
}

void DRFrontend::event_exit(void)
{
  if (m_options->get_verbose())
  {
    std::cerr << "[Exit event] Begin."  << std::endl;
  }

  if (!dr_raw_tls_cfree(tls_offs, FRONTEND_TLS_COUNT))
    DR_ASSERT(false);
        
  drvector_delete(&magic_free_regs);
  if (drtime::supported())
  {
    drmgr_unregister_module_load_event(event_module_load);
    drmgr_unregister_bb_app2app_event(event_app2app);
    drmgr_unregister_signal_event(event_signal);
  }
  drtime::exit();
  if (!drmgr_unregister_tls_field(tls_idx) ||
      !drmgr_unregister_thread_init_event(event_thread_init) ||
      !drmgr_unregister_thread_exit_event(event_thread_exit)||
      !drmgr_unregister_bb_insertion_event(event_app_instruction) ||
      drreg_exit() != DRREG_SUCCESS)
    DR_ASSERT(false);

  // Debug DynamoRIO reports a client leak, and exits with status 255, for
  // anything still allocated here
  dr_mutex_destroy(thread_lock);
  thread_lock = NULL;

  drmgr_exit();
  if (m_options->get_verbose())
  {
    std::cerr << "[Exit event] End."  << std::endl;
  }
}

// analysis of application code: count instructions 
// a list of instructions passed
dr_emit_flags_t DRFrontend::event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                  bool for_trace, bool translating, void **user_data)
{
    uint num_instrs = 0;

    // DynamoRIO builds a new block for code written since it was last
    // translated: tell the writers when the bytes differ from the ones they
    // sent (Sift::CodeTracker).  Block-building time only; traces are made
    // of blocks already seen, and translating only recreates state.
    if (Sift::CodeTracker::enabled() && !for_trace && !translating)
    {
      for (instr_t *instr = instrlist_first_app(bb); instr != NULL; instr = instr_get_next_app(instr))
      {
        app_pc pc = instr_get_app_pc(instr);
        int len = instr_length(drcontext, instr);
        uint8_t bytes[16];
        size_t got = 0;
        if (pc != NULL && len > 0 && len <= 16 && dr_safe_read(pc, len, bytes, &got))
          Sift::CodeTracker::observe((uint64_t)pc, bytes, (uint32_t)got);
      }
    }

    // Only count instructions if we are not in detailed simulation
    if (m_control->get_any_thread_in_detail())
      return DR_EMIT_DEFAULT;

    // Only count in app BBs
    // TODO: is this necessary? Probably not (excludes libraries)
    module_data_t *mod = dr_lookup_module(dr_fragment_app_pc(tag));
    if (mod != NULL) 
    {
      bool from_exe = (mod->start == exe_start);
      dr_free_module_data(mod);
      if (!from_exe) 
      {
        *user_data = NULL;
        return DR_EMIT_DEFAULT;
      }
    }
    
    // Count instructions in current basic block
    for (instr_t *instr = instrlist_first_app(bb);
         instr != NULL;
         instr = instr_get_next_app(instr)) {
        num_instrs++;
    }
    *user_data = (void *)(ptr_uint_t)num_instrs;

    return DR_EMIT_DEFAULT;
}

// hold_last: the last instruction recorded has not run (it will be recorded
// again): it is not sent, only looked at for the branch before it
void DRFrontend::process_instructions_buffer(void *drcontext, bool hold_last)
{
  per_thread_t *data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
  instruction_t *buf_ptr = BUF_PTR(data->seg_base);
  instruction_t *buf_end = hold_last && buf_ptr > data->inst_buf ? buf_ptr - 1 : buf_ptr;
  // The instructions the thread ran, for its clock
  drtime::count(data->fe_tid, buf_end - data->inst_buf);
  
  for (instruction_t *instruction = (instruction_t *)data->inst_buf; instruction < buf_end; instruction++) 
  {
    // Update branch information
    bool taken = false;
    if (instruction->is_branch && (instruction+1) < buf_ptr){
      // Check next instruction's address to see if the branch has been taken
      ptr_uint_t next_address = (ptr_uint_t)(instruction+1)->pc;
      // Compare with the recorded value for the taken path
      auto t = data->taken->find((ptr_uint_t)instruction->pc);
      taken = t != data->taken->end() && t->second == next_address;
    }
    
/*    dr_fprintf(STDERR, "[%d] " PFX "(%d):%d[%d],b:%d,t:%d,p:%d,e:%d,a:%d,z:%d\n", instruction->threadid, (ptr_uint_t)instruction->pc, instruction->isize,
                                              instruction->num_addresses, instruction->ndynaddr, instruction->is_branch, taken,
                                              instruction->is_predicate, instruction->is_executing, 
                                              instruction->is_before, instruction->is_pause);*/
    
    // Update dynamic addresses information in the thread data structure      
    // (at most Sift::MAX_DYNAMIC_ADDRESSES, 2: addMemoryModeling asserts it
    // when it instruments the instruction)
    m_thread_data[instruction->threadid].num_dyn_addresses = instruction->ndynaddr;
    if (instruction->ndynaddr > 0)
      m_thread_data[instruction->threadid].dyn_addresses[0] = (ptr_uint_t)instruction->dynaddr_0;
    if (instruction->ndynaddr > 1)
      m_thread_data[instruction->threadid].dyn_addresses[1] = (ptr_uint_t)instruction->dynaddr_1;
    
    // Do we have to change the ISA mode? (ARM-32 only)
    IF_ARM
    (
      if(last_mode != instruction->isa_mode)
      {
        const int isa = sniperIsaOf(instruction->isa_mode);
        if (isa < 0)
        {
          dr_fprintf(STDERR, "[SNIPER_FRONTEND] Error: the application runs in DynamoRIO ISA mode %d "
                             "(dr_isa_mode_t), which Sniper cannot simulate\n", instruction->isa_mode);
          dr_abort();
        }
        m_callbacks->changeISA((threadid_t)instruction->threadid, isa);
        last_mode = instruction->isa_mode;
      }
    )
    
    m_callbacks->sendInstruction((threadid_t)instruction->threadid, (addr_t)instruction->pc, instruction->isize,
                                                instruction->num_addresses, instruction->is_branch, 
                                                taken, instruction->is_predicate, 
                                                instruction->is_executing, instruction->is_before, instruction->is_pause);
    instruction->ndynaddr = 0;
  }
  BUF_PTR(data->seg_base) = data->inst_buf;

}

// A signal for the thread that waits in DynamoRIO (drtime::flow: a timer
// fired here, from a clean call, where DynamoRIO only notes it, or the
// client thread signalled it while it was in one): DynamoRIO delivers a
// signal as the thread leaves the code cache, and a thread computing in
// linked blocks never did -- the SIGALRM of an ITIMER_REAL armed before a
// loop never came.  So the thread leaves: back to the start of its block
// (pc), through DynamoRIO, which delivers the signal first.  The block's
// first instruction, recorded before the clean call but not run, is not
// sent: it is recorded again.
void DRFrontend::resume_at(void *drcontext, app_pc pc)
{
  dr_mcontext_t mc;
  mc.size = sizeof(mc);
  mc.flags = DR_MC_ALL;
  dr_get_mcontext(drcontext, &mc);
  mc.pc = pc;
  dr_redirect_execution(&mc);
}

void DRFrontend::clean_call(app_pc pc)
{
  void *drcontext = dr_get_current_drcontext();
  if (drtime::flow(fe_tid(drcontext)))
  {
    process_instructions_buffer(drcontext, true);
    resume_at(drcontext, pc);
  }
  process_instructions_buffer(drcontext);
}

void DRFrontend::count_clean_call(threadid_t threadid, int32_t count, app_pc pc)
{
  if (drtime::flow(threadid))
    resume_at(dr_get_current_drcontext(), pc);   // the block is counted when it runs again
  m_callbacks->countInsns(threadid, count);
  drtime::count(threadid, count);
}

// ---------------------------------------------------------------- the guest's time
//
// glibc reads the clocks in the vDSO, with rdtsc and the kernel's data page:
// no system call, so the host's time.  The vDSO's clock_gettime,
// gettimeofday and time are replaced, as the SIFT recorder replaces the
// libc routines: the block at each entry becomes a clean call and a ret
// (event_app2app).  The clean call writes the guest's time (dr_fe_time.cc:
// the simulator's, live, else the one-IPC model's) where the caller asked
// and the result in RAX; the ret returns to the caller.  Neither is traced:
// the replaced routine runs no instruction of the guest's.  (Returning from
// the clean call with dr_redirect_execution cost 60 us a call.)  rdtsc and
// rdtscp are left to run, their result replaced after them by the guest's
// TSC.

enum { VDSO_CLOCK_GETTIME, VDSO_GETTIMEOFDAY, VDSO_TIME, VDSO_COUNT };
static app_pc vdso_entry[VDSO_COUNT][2];   // __vdso_<name> and its alias <name>

static int vdso_which(app_pc pc)
{
  for (int i = 0; i < VDSO_COUNT; i++)
    if (pc != NULL && (vdso_entry[i][0] == pc || vdso_entry[i][1] == pc))
      return i;
  return -1;
}

void DRFrontend::event_module_load(void *drcontext, const module_data_t *info, bool loaded)
{
  const char *name = dr_module_preferred_name(info);
  if (name == NULL || strstr(name, "vdso") == NULL)
    return;
  // x86-64's names, else AArch64's (whose vDSO has no time)
  static const char *names[VDSO_COUNT][2] = {
    { "__vdso_clock_gettime", "clock_gettime" },
    { "__vdso_gettimeofday", "gettimeofday" },
    { "__vdso_time", "time" },
  };
  static const char *names_a64[VDSO_COUNT] = { "__kernel_clock_gettime", "__kernel_gettimeofday", NULL };
  for (int i = 0; i < VDSO_COUNT; i++)
    for (int j = 0; j < 2; j++)
    {
      vdso_entry[i][j] = (app_pc)dr_get_proc_address(info->handle, names[i][j]);
      if (vdso_entry[i][j] == NULL && j == 0 && names_a64[i] != NULL)
        vdso_entry[i][j] = (app_pc)dr_get_proc_address(info->handle, names_a64[i]);
    }
}

void DRFrontend::vdso_clean_call(int which)
{
#if defined(X86_64)   // x86-64's registers and calling convention (and only there drtime::supported())
  void *drcontext = dr_get_current_drcontext();
  process_instructions_buffer(drcontext);   // the clock counts them
  const threadid_t threadid = fe_tid(drcontext);
  dr_mcontext_t mc;
  mc.size = sizeof(mc);
  mc.flags = DR_MC_INTEGER;
  dr_get_mcontext(drcontext, &mc);
  reg_t ret = 0;
  uint64_t ns = 0;
  size_t done;
  if (which == VDSO_CLOCK_GETTIME)
  {
    if (!drtime::clock_ns(threadid, (int64_t)(int32_t)mc.xdi, &ns))
      ret = (reg_t)-EINVAL;
    else if (mc.xsi)
    {
      const uint64_t ts[2] = { ns / Sift::NS_PER_S, ns % Sift::NS_PER_S };
      if (!dr_safe_write((void *)mc.xsi, sizeof(ts), ts, &done))
        ret = (reg_t)-EFAULT;
    }
  }
  else
  {
    drtime::clock_ns(threadid, 0, &ns);
    if (which == VDSO_GETTIMEOFDAY)
    {
      const uint64_t tv[2] = { ns / Sift::NS_PER_S, ns / 1000 % 1000000 };
      const int32_t tz[2] = { 0, 0 };
      if ((mc.xdi && !dr_safe_write((void *)mc.xdi, sizeof(tv), tv, &done)) ||
          (mc.xsi && !dr_safe_write((void *)mc.xsi, sizeof(tz), tz, &done)))
        ret = (reg_t)-EFAULT;
    }
    else
    {
      const uint64_t s = ns / Sift::NS_PER_S;
      ret = (reg_t)s;
      if (mc.xdi && !dr_safe_write((void *)mc.xdi, sizeof(s), &s, &done))
        ret = (reg_t)-EFAULT;
    }
  }
  // The result in RAX; the block's ret returns to the caller
  mc.xax = ret;
  dr_set_mcontext(drcontext, &mc);
#endif
}

dr_signal_action_t DRFrontend::event_signal(void *drcontext, dr_siginfo_t *siginfo)
{
  drtime::signal_delivered(fe_tid(drcontext));
  return DR_SIGNAL_DELIVER;
}

void DRFrontend::rdtsc_clean_call()
{
#if defined(X86_64)
  void *drcontext = dr_get_current_drcontext();
  process_instructions_buffer(drcontext);
  dr_mcontext_t mc;
  mc.size = sizeof(mc);
  mc.flags = (dr_mcontext_flags_t)(DR_MC_INTEGER | DR_MC_CONTROL);
  dr_get_mcontext(drcontext, &mc);
  const uint64_t t = drtime::tsc(fe_tid(drcontext));
  mc.xax = (reg_t)(t & 0xffffffff);
  mc.xdx = (reg_t)(t >> 32);
  dr_set_mcontext(drcontext, &mc);
#endif
}

static bool is_rdtsc(instr_t *instr)
{
  return IF_X86_ELSE(instr_get_opcode(instr) == OP_rdtsc || instr_get_opcode(instr) == OP_rdtscp, false);
}

// A read of the host's counter or its rate: rdtsc, or AArch64's mrs of
// CNTVCT_EL0, CNTPCT_EL0 or CNTFRQ_EL0; 0 for another instruction, else
// 1 + the register it writes (AArch64), or 1 (rdtsc: EDX:EAX)
enum { COUNTER_NONE = 0, COUNTER_VALUE = 1, COUNTER_RATE = 2 };
static int counter_read(instr_t *instr, reg_id_t *dst)
{
#ifdef AARCH64
  if (instr_get_opcode(instr) == OP_mrs && instr_num_srcs(instr) > 0 && opnd_is_reg(instr_get_src(instr, 0))
      && instr_num_dsts(instr) > 0 && opnd_is_reg(instr_get_dst(instr, 0)))
  {
    const reg_id_t r = opnd_get_reg(instr_get_src(instr, 0));
    *dst = opnd_get_reg(instr_get_dst(instr, 0));
    if (r == DR_REG_CNTVCT_EL0 || r == DR_REG_CNTPCT_EL0)
      return COUNTER_VALUE;
    if (r == DR_REG_CNTFRQ_EL0)
      return COUNTER_RATE;
  }
  return COUNTER_NONE;
#else
  *dst = DR_REG_NULL;
  return is_rdtsc(instr) ? COUNTER_VALUE : COUNTER_NONE;
#endif
}

// Where the guest's time is not kept: a vDSO clock ends the run
// (drtime::refuse)
void DRFrontend::refuse_time_clean_call(int which)
{
  static const char *what[] = { "the vDSO's clock_gettime", "the vDSO's gettimeofday", "the vDSO's time" };
  drtime::refuse(what[which >= 0 && which < VDSO_COUNT ? which : 0]);
}

// Where the guest's time is not kept, after a counter read: the
// instructions run at 1 GHz (drtime::counter_ticks), or that rate, in place
// of the host's.  dst: the register written (AArch64), DR_REG_NULL for
// rdtsc's EDX:EAX
void DRFrontend::counter_clean_call(int kind, int dst)
{
  void *drcontext = dr_get_current_drcontext();
  process_instructions_buffer(drcontext);   // the count includes them
  dr_mcontext_t mc;
  mc.size = sizeof(mc);
  mc.flags = DR_MC_INTEGER;
  dr_get_mcontext(drcontext, &mc);
  const uint64_t v = kind == COUNTER_RATE ? drtime::COUNTER_HZ : drtime::counter_ticks(fe_tid(drcontext));
#ifdef X86_64
  if (dst == DR_REG_NULL)
  {
    mc.xax = (reg_t)(v & 0xffffffff);
    mc.xdx = (reg_t)(v >> 32);
  }
  else
#endif
    reg_set_value((reg_id_t)dst, &mc, (reg_t)v);
  dr_set_mcontext(drcontext, &mc);
}

static int vdso_block(void *tag)
{
  return vdso_which(dr_fragment_app_pc(tag));
}

// The block at a vDSO clock's entry becomes a ret, before which the clean
// call goes (event_app_instruction).  An rdtsc ends its block, so that its
// clean call follows it at the block's end, where no scratch register is
// held.
dr_emit_flags_t DRFrontend::event_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating)
{
#if defined(X86_64)
  if (!drtime::refusing() && vdso_block(tag) >= 0)
  {
    instrlist_clear(drcontext, bb);
    instr_t *ret = INSTR_CREATE_ret(drcontext);
    instr_set_translation(ret, dr_fragment_app_pc(tag));
    instrlist_append(bb, ret);
    return DR_EMIT_DEFAULT;
  }
#endif
  for (instr_t *instr = instrlist_first_app(bb); instr != NULL; instr = instr_get_next_app(instr))
  {
    reg_id_t dst;
    if (!(drtime::refusing() ? counter_read(instr, &dst) != COUNTER_NONE : is_rdtsc(instr)))
      continue;
    instr_t *next;
    while ((next = instr_get_next(instr)) != NULL)
    {
      instrlist_remove(bb, next);
      instr_destroy(drcontext, next);
    }
    break;
  }
  return DR_EMIT_DEFAULT;
}

void DRFrontend::insert_load_buf_ptr(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t reg_ptr)
{
  dr_insert_read_raw_tls(drcontext, bb, instr, tls_seg, tls_offs + FRONTEND_TLS_OFFS_BUF_PTR, reg_ptr);
}

void DRFrontend::insert_update_buf_ptr(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t reg_ptr, int adjust)
{
  instrlist_meta_preinsert( bb, instr,
                            XINST_CREATE_add(drcontext, opnd_create_reg(reg_ptr), OPND_CREATE_INT16(adjust))
                          );
  dr_insert_write_raw_tls(drcontext, bb, instr, tls_seg, tls_offs + FRONTEND_TLS_OFFS_BUF_PTR, reg_ptr);
}

void DRFrontend::insert_save_pc(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t base, reg_id_t scratch, app_pc pc)
{
  instrlist_insert_mov_immed_ptrsz(drcontext, (ptr_int_t)pc, opnd_create_reg(scratch), bb, instr, NULL, NULL);
  instrlist_meta_preinsert( bb, instr,
                            XINST_CREATE_store( drcontext, 
                                                OPND_CREATE_MEMPTR(base, offsetof(instruction_t, pc)),
                                                opnd_create_reg(scratch)
                                              )
                          );
}

void DRFrontend::insert_save_int(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t base, reg_id_t scratch, int disp, int value)
{
  scratch = reg_resize_to_opsz(scratch, OPSZ_2);
  instrlist_meta_preinsert(bb, instr,
                           XINST_CREATE_load_int(drcontext, opnd_create_reg(scratch), OPND_CREATE_INT16(value))
                          );
  instrlist_meta_preinsert(bb, instr,
                           XINST_CREATE_store_2bytes(drcontext, 
                                                     OPND_CREATE_MEM16(base, disp),
                                                     opnd_create_reg(scratch))
                          );  
}

void DRFrontend::handleMemory(threadid_t threadid, void *drcontext, instrlist_t *bb, instr_t *instr, 
                                            reg_id_t base, reg_id_t scratch, ptr_uint_t address, int disp_dynaddr)
{

  // We're still called for instructions in the same basic block as ROI end, ignore these
  if (!m_thread_data[threadid].output)
    return;

  // Save value of dynamic address in alternative buffer (not yet the one of thread_data)
  // move value of address to save to a temporary register
 /* instrlist_insert_mov_immed_ptrsz(drcontext, address, opnd_create_reg(scratch), bb, instr, NULL, NULL);
  // read current value of num dyn addresses
  instrlist_meta_preinsert( bb, instr,
                            XINST_CREATE_store( drcontext, 
                                                OPND_CREATE_MEMPTR(base, disp_dynaddr),
                                                opnd_create_reg(scratch)
                                              )
                          );*/
  // Insert meta instructions to increment the value of num_dyn_addresses in runtime
  // Load current value of num_dyn_addresses in instruction_t struct to a scratch register
  instrlist_meta_preinsert(bb, instr,
                           XINST_CREATE_load(drcontext, opnd_create_reg(scratch), 
                                             OPND_CREATE_MEMPTR(base, offsetof(instruction_t, ndynaddr))));
  // Increment value in scratch register
  instrlist_meta_preinsert(bb, instr, XINST_CREATE_add(drcontext, opnd_create_reg(scratch), OPND_CREATE_INT32(1)));
  // Save incremented value back to num_dyn_addresses
  instrlist_meta_preinsert(bb, instr,
                           XINST_CREATE_store(drcontext, 
                                              OPND_CREATE_MEMPTR(base, offsetof(instruction_t, ndynaddr)),
                                              opnd_create_reg(scratch))); 
}

unsigned int DRFrontend::addMemoryModeling( threadid_t threadid, void *drcontext, instrlist_t *bb, instr_t *instr, 
                                            reg_id_t base, reg_id_t scratch)
{
  unsigned int num_addresses = 0;
  int current_disp = offsetof(instruction_t, dynaddr_0);

  // Insert meta instruction that initializes number of dynamic addresses to 0 in runtime
  insert_save_int(drcontext, bb, instr, base, scratch, offsetof(instruction_t, ndynaddr), num_addresses);
   
  if (instr_reads_memory(instr) || instr_writes_memory(instr))
  {
    for (int i = 0; i < instr_num_srcs(instr); i++) 
    {
      if (opnd_is_memory_reference(instr_get_src(instr, i)))
      {
        bool ok;
        ok = drutil_insert_get_mem_addr(drcontext, bb, instr, instr_get_src(instr, i), scratch, base);
        DR_ASSERT(ok);
        insert_load_buf_ptr(drcontext, bb, instr, base);
        instrlist_meta_preinsert(bb, instr,
            XINST_CREATE_store(drcontext,
                               OPND_CREATE_MEMPTR(base, current_disp),
                                                  opnd_create_reg(scratch)));      
                                                       
        handleMemory(threadid, drcontext, bb, instr, base, scratch, 0, 0);
        num_addresses++;
        // update offset displacement for the next address to save
        switch(num_addresses)
        {
            case 1:
              current_disp = offsetof(instruction_t, dynaddr_1);
              break;
            case 2:
              current_disp = offsetof(instruction_t, dynaddr_2);
        }
      }
    }

    for (int i = 0; i < instr_num_dsts(instr); i++) {
        if (opnd_is_memory_reference(instr_get_dst(instr, i)))
        {
          bool ok;
          ok = drutil_insert_get_mem_addr(drcontext, bb, instr, instr_get_dst(instr, i), scratch, base);
          DR_ASSERT(ok);
          insert_load_buf_ptr(drcontext, bb, instr, base);
          instrlist_meta_preinsert(bb, instr,
              XINST_CREATE_store(drcontext,
                                OPND_CREATE_MEMPTR(base, current_disp),
                                                   opnd_create_reg(scratch)));   

     
          handleMemory(threadid, drcontext, bb, instr, base, scratch, 0, 0);
          num_addresses++;
          // update offset displacement for the next address to save
          switch(num_addresses)
          {
            case 1:
              current_disp = offsetof(instruction_t, dynaddr_1);
              break;
            case 2:
              current_disp = offsetof(instruction_t, dynaddr_2);
          }
        }
    }
  }
  sift_assert(num_addresses <= Sift::MAX_DYNAMIC_ADDRESSES);

  return num_addresses;
}

void DRFrontend::magic_clean_call()
{
  void * drcontext = dr_get_current_drcontext();
  /* The instructions before this one first: they wait in the trace buffer
     until it fills, and a magic record sent ahead of them reached the
     simulator early -- SimMagic1(SIM_CMD_GET_INS_NUM) read a count short of
     the buffered instructions, an ROI began before code that precedes it,
     a marker fell in the wrong place.  The SDE and QEMU front ends send
     instructions as they go. */
  process_instructions_buffer(drcontext);
  dr_mcontext_t mc = {};
  mc.size = sizeof(mc);
  mc.flags = DR_MC_ALL;
  dr_get_mcontext(drcontext, &mc);
  /* The simulator's answer goes into the result register before the magic
     instruction runs (it leaves that register alone), as the SDE and QEMU
     front ends do.  It was dropped here, so under DynamoRIO SimInSimulator()
     said no and every query returned its own command number. */
  IF_X86
  (
    // Invoke the handleMagic callback with command (RAX) and arguments (RBX, RCX); the answer in RAX
    reg_t res = m_callbacks->handleMagic(fe_tid(drcontext),
                           reg_get_value(DR_REG_RAX, &mc),
                           reg_get_value(DR_REG_RBX, &mc),
                           reg_get_value(DR_REG_RCX, &mc) );
    reg_set_value(DR_REG_RAX, &mc, res);
  )
  IF_AARCH64
  (
    // Invoke the handleMagic callback with command (X1) and arguments (X2, X3); the answer in X0
    reg_t res = m_callbacks->handleMagic(fe_tid(drcontext),
                           reg_get_value(DR_REG_X1, &mc),
                           reg_get_value(DR_REG_X2, &mc),
                           reg_get_value(DR_REG_X3, &mc) );
    reg_set_value(DR_REG_X0, &mc, res);
  )
  dr_set_mcontext(drcontext, &mc);
}

void DRFrontend::invoke_endROI()
{
  void * drcontext = dr_get_current_drcontext();
  m_control->endROI(fe_tid(drcontext));
}

// instrumentation insertion -- after analysis
dr_emit_flags_t DRFrontend::event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
                      bool for_trace, bool translating, void *user_data)
{
    // ignore tool-inserted instrumentation
    if (!instr_is_app(instr))
      return DR_EMIT_DEFAULT;
      
    // Where the guest's time is not kept: a vDSO clock ends the run before
    // it runs; a counter read reads the instructions run, after it (the
    // block's last instruction: event_app2app)
    if (drtime::refusing())
    {
      const int which = vdso_block(tag);
      reg_id_t dst;
      int kind;
      if (which >= 0 && drmgr_is_first_instr(drcontext, instr))
        dr_insert_clean_call(drcontext, bb, instr, (void *)refuse_time_clean_call, false, 1, OPND_CREATE_INT32(which));
      else if ((kind = counter_read(instr, &dst)) != COUNTER_NONE)
        dr_insert_clean_call(drcontext, bb, NULL, (void *)counter_clean_call, false, 2,
                             OPND_CREATE_INT32(kind), OPND_CREATE_INT32((int)dst));
    }
    // A vDSO clock: the clean call before the block's ret (event_app2app),
    // and nothing traced
    else
    {
      const int which = vdso_block(tag);
      if (which >= 0)
      {
        if (drmgr_is_first_instr(drcontext, instr))
          dr_insert_clean_call(drcontext, bb, instr, (void *)vdso_clean_call, false, 1, OPND_CREATE_INT32(which));
        return DR_EMIT_DEFAULT;
      }
    }
    // rdtsc: its result replaced by the guest's TSC, after it (the block's
    // last instruction: event_app2app)
    if (!drtime::refusing() && is_rdtsc(instr))
      dr_insert_clean_call(drcontext, bb, NULL, (void *)rdtsc_clean_call, false, 0);

    // Check Magic instructions
    if (is_magic_instr(instr))
      dr_insert_clean_call(drcontext, bb, instr, (void *)magic_clean_call, false, 0);

    // Check if the stop address has been reached and end simulation 
    if (m_control->get_stop_address() && ( (ptr_uint_t)instr_get_app_pc(instr) == m_control->get_stop_address()))
    {
      dr_insert_clean_call( drcontext, bb, instr, (void *)invoke_endROI, false, 0);
    }
      
    // Are we in fast forward mode? Then just count instructions
    if (!m_control->get_any_thread_in_detail())
    {
      uint num_instrs;
      //std::cerr << "Not detailed" << std::endl;
      // Ignore if this isn't the first instruction of the BB
      if (!drmgr_is_first_instr(drcontext, instr))
        return DR_EMIT_DEFAULT;
        
      // Only insert count calls for in-app BBs
      if (user_data == NULL)
        return DR_EMIT_DEFAULT;
    
      // Get parameters to pass to countInsns
      // 1: get threadid       
      int threadid = fe_tid(drcontext);  
      // 2: user_data with the number of instructions has been updated in event_bb_analysis
      num_instrs = (uint)(ptr_uint_t)user_data;

      // Insert clean call to count instructions
      dr_insert_clean_call( drcontext, bb, instrlist_first_app(bb),
                            (void *)count_clean_call, // the frontend's countInsns, and the guest's clock
                            false, // save fpstate 
                            3, // number of operands
                            OPND_CREATE_INT32(threadid),
                            OPND_CREATE_INT32(num_instrs),
                            OPND_CREATE_INTPTR(instr_get_app_pc(instrlist_first_app(bb))));
    }
    else  // detailed simulation: send instructions to Sniper's backend
    {
      // Reserve two scratch registers
      reg_id_t reg_ptr, reg_tmp;
      drvector_t *allowed = scratch_allowed(bb);
      if (drreg_reserve_register(drcontext, bb, instr, allowed, &reg_ptr) != DRREG_SUCCESS ||
          drreg_reserve_register(drcontext, bb, instr, allowed, &reg_tmp) != DRREG_SUCCESS)
            DR_ASSERT(false); // cannot recover
      
      // Load the address to the buffer to save the collected information of current instruction
      insert_load_buf_ptr(drcontext, bb, instr, reg_ptr);
      
      // Collect instruction's info to send to the backend using meta instructions
      // 1: current thread's id
      int tid = fe_tid(drcontext); 
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, threadid), tid);

      // 2: address of this instruction 
      app_pc address = instr_get_app_pc(instr);  // app_pc is a byte*
      insert_save_pc(drcontext, bb, instr, reg_ptr, reg_tmp, address);

      // 3: size of this instruction
      int instr_size = instr_length(drcontext, instr);
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, isize), instr_size);

      // 4: memory modeling to get the number of addresses and to register the addresses of the
      // memory operands accessed at runtime
      int naddresses = addMemoryModeling(tid, drcontext, bb, instr, reg_ptr, reg_tmp);
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, num_addresses), naddresses);

      // 5: is this instruction a conditional branch (can cause misprediction penalties)?
      bool this_is_branch = instr_is_cbr(instr);
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, is_branch), this_is_branch);

      // 6: if this is a branch, is it taken? Insert information to process taken/not taken later when we do 
      // the sendInstruction.
      if (this_is_branch)
      {
        app_pc targ = instr_get_branch_target_pc(instr);
        ((per_thread_t *) drmgr_get_tls_field(drcontext, tls_idx))->taken->insert_or_assign((ptr_uint_t)address, (ptr_uint_t)targ);
        // This part used to be implemented with a dedicated callback, but it is not supported for ARM
        //dr_insert_cbr_instrumentation_ex(drcontext, bb, instr, (void *)at_cbr, OPND_CREATE_INT32(tid)); 
      } 

      // 7: is this instruction predicated?
      bool this_is_predicated = false;//IF_AARCH64_ELSE(false, instr_is_predicated(instr));
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, is_predicate), this_is_predicated);

      // 8: false if the instruction will not be executed because of predication
      bool this_is_executing = true;
      if (this_is_predicated) {
        // get the machine context
        dr_mcontext_t mcontext = {};
        mcontext.size = sizeof(mcontext);
        mcontext.flags = DR_MC_ALL;
        dr_get_mcontext(drcontext, &mcontext);
        // Will the predicated instruction instr execute under current machine context?
        this_is_executing = (instr_predicate_triggered(instr, &mcontext) 
                        == DR_PRED_TRIGGER_MATCH);  // The predicate matches and the instruction will execute
      }
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, is_executing), this_is_executing);

      // 9: instrumentation inserted before the instruction?
      bool this_is_before = true;
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, is_before), this_is_before);

      // 10: if this instruction is X86, is it a 'pause' instruction?
      bool this_is_pause = IF_X86_ELSE(instr_get_opcode(instr) == OP_pause, false);
      insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, is_pause), this_is_pause);
      
      // 11: ISA mode -- save only if we are in an ARM machine to distinguish between ARM and Thumb instructions
      IF_ARM
      (
        dr_isa_mode_t current_isa_mode = instr_get_isa_mode(instr);
        insert_save_int(drcontext, bb, instr, reg_ptr, reg_tmp, offsetof(instruction_t, isa_mode), current_isa_mode);
      )
      
      // Update buffer's pointer for next instruction
      insert_update_buf_ptr(drcontext, bb, instr, reg_ptr, sizeof(instruction_t));

      // Restore scratch registers
      if (drreg_unreserve_register(drcontext, bb, instr, reg_ptr) != DRREG_SUCCESS || 
          drreg_unreserve_register(drcontext, bb, instr, reg_tmp) != DRREG_SUCCESS)
            DR_ASSERT(false);
  
      // Insert clean call to send this instruction to Sniper's backend
      if (drmgr_is_first_instr(drcontext, instr) &&
          IF_ARM_ELSE(!instr_is_predicated(instr), true)
          IF_AARCHXX(&& !instr_is_exclusive_store(instr)))
      {
        dr_insert_clean_call(drcontext, bb, instr, (void *)clean_call, false, 1,
                             OPND_CREATE_INTPTR(instr_get_app_pc(instr)));
      }
      
      
    }

    return DR_EMIT_DEFAULT;
}

//
// DynamoRIO's client main function
//
DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
  //std::cerr << "Main client" << std::endl;
  frontend::ExecFrontend<DRFrontend>(argc, argv).start();
}


