#include "dr_frontend.h"

#include "drmgr.h"
#include "drutil.h"
#include "drreg.h"

#include "dr_fe_options.h"
#include "dr_fe_copy.h"
#include "dr_fe_syscall.h"
#include "dr_fe_lock.h"
#include "dr_fe_control.h"
#include "dr_fe_threads.h"

// Static fields
std::unordered_map<int, threadid_t> DRFrontend::map_threadids;
std::vector<std::unordered_map<ptr_uint_t, ptr_uint_t>> DRFrontend::map_taken_branch;
threadid_t DRFrontend::next_threadid = 0;
app_pc DRFrontend::exe_start;
int DRFrontend::tls_idx;
reg_id_t DRFrontend::tls_seg;
uint DRFrontend::tls_offs;
app_pc DRFrontend::last_opnd;
int DRFrontend::last_mode = -1;

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
  std::cerr << "Inside allocate" << std::endl;
  m_thread_data = (thread_data_t*) dr_custom_alloc(NULL, (dr_alloc_flags_t) 0, thread_data_size, 0, NULL);
}

// Frontend initialization, called from the specialization of ExecFrontend::handle_frontend_init().
void DRFrontend::init()
{
  std::cerr << "Init specific frontend" << std::endl;

  this->num_threads = 0;
  // Initialize options for scratch registers - We need 2 reg slots beyond drreg's eflags slots => 3 slots 
  drreg_options_t ops = {sizeof(ops), 3, false};  

  if(!drmgr_init() || drreg_init(&ops) != DRREG_SUCCESS)
    DR_ASSERT(false);
    
  dr_set_client_name("Sniper's frontend based on DynamoRIO", "http://snipersim.org");
  
  // Get main module address
  module_data_t *exe = dr_get_main_module();
  if (exe != NULL)
    this->exe_start = exe->start;
  dr_free_module_data(exe);

  // Init syscall emulation if response files
  if (DREmulateSyscalls.get_value()) 
  {
    if (!DRUseResponseFiles.get_value())
    {
      std::cerr << "Error, Response files are required when using syscall emulation." << std::endl;
      exit(1);
    }

    m_sysmodel->set_map_threads(&map_threadids);
    m_sysmodel->initSyscallModeling();
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

  // get DR's internal threadid
  int threadid_dr = dr_get_thread_id(drcontext);  
  // add new thread to the mapping of Frontend <-> DR threadids
  map_threadids[threadid_dr] = next_threadid;
  // create a new map of branches taken or not taken
  std::unordered_map<ptr_uint_t, ptr_uint_t> bt;
  map_taken_branch.push_back(bt);

  // Call the generic frontend callback for a thread start
  m_threads->threadStart(next_threadid);

  // Update the thread counter
  next_threadid++;
    std::cerr << "[Thread init end] Threadid: " << next_threadid << std::endl;

}

void DRFrontend::event_thread_exit(void *drcontext)
{
  process_instructions_buffer(drcontext); // dump remaining contents
  
  // get DR's internal threadid
  int threadid_dr = dr_get_thread_id(drcontext);  
  // get thread id in the frontend of this DR threadid
  threadid_t threadid_fe = map_threadids[threadid_dr];
  std::cerr << "[Thread exit] Threadid: " << threadid_fe << std::endl;
  if(dr_using_all_private_caches())
      std::cerr << "[Thread exit] Using private caches."  << std::endl;
  
  // free allocated memory for tls 
  per_thread_t *data = (per_thread_t *) drmgr_get_tls_field(drcontext, tls_idx);
  dr_raw_mem_free(data->inst_buf, MEM_BUF_SIZE);
  dr_thread_free(drcontext, data, sizeof(per_thread_t));

  m_threads->threadFinish(threadid_fe, 0);  // TODO are flags here relevant?
}

void DRFrontend::event_exit(void)
{
  std::cerr << "[Exit event] Begin."  << std::endl;

  if (!dr_raw_tls_cfree(tls_offs, FRONTEND_TLS_COUNT))
    DR_ASSERT(false);
        
  if (!drmgr_unregister_tls_field(tls_idx) ||
      !drmgr_unregister_thread_init_event(event_thread_init) ||
      !drmgr_unregister_thread_exit_event(event_thread_exit)||
      !drmgr_unregister_bb_insertion_event(event_app_instruction) ||
      drreg_exit() != DRREG_SUCCESS)
    DR_ASSERT(false);
    
  drmgr_exit();
  std::cerr << "[Exit event] End."  << std::endl;
}

// analysis of application code: count instructions 
// a list of instructions passed
dr_emit_flags_t DRFrontend::event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                  bool for_trace, bool translating, void **user_data)
{
    uint num_instrs = 0;
    
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

void DRFrontend::process_instructions_buffer(void *drcontext)
{
  per_thread_t *data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
  instruction_t *buf_ptr = BUF_PTR(data->seg_base);
  
  for (instruction_t *instruction = (instruction_t *)data->inst_buf; instruction < buf_ptr; instruction++) 
  {
    // Update branch information
    bool taken = false;
    if (instruction->is_branch && (instruction+1) < buf_ptr){
      // Check next instruction's address to see if the branch has been taken
      ptr_uint_t next_address = (ptr_uint_t)(instruction+1)->pc;
      // Compare with the recorded value for the taken path
      taken = map_taken_branch[instruction->threadid][(ptr_uint_t)instruction->pc] == next_address;
    }
    
/*    dr_fprintf(STDERR, "[%d] " PFX "(%d):%d[%d],b:%d,t:%d,p:%d,e:%d,a:%d,z:%d\n", instruction->threadid, (ptr_uint_t)instruction->pc, instruction->isize,
                                              instruction->num_addresses, instruction->ndynaddr, instruction->is_branch, taken,
                                              instruction->is_predicate, instruction->is_executing, 
                                              instruction->is_before, instruction->is_pause);*/
    
    // Update dynamic addresses information in the thread data structure      
    m_thread_data[instruction->threadid].num_dyn_addresses = instruction->ndynaddr;
    for (unsigned int i = 0; i < m_thread_data[instruction->threadid].num_dyn_addresses; i++)
    {
      switch(i)
      {
        case 0:
          m_thread_data[instruction->threadid].dyn_addresses[0] = (ptr_uint_t)instruction->dynaddr_0;
          break;
        case 1:
          m_thread_data[instruction->threadid].dyn_addresses[1] = (ptr_uint_t)instruction->dynaddr_1;
          break;
        case 2:
          m_thread_data[instruction->threadid].dyn_addresses[2] = (ptr_uint_t)instruction->dynaddr_2;
          break;        
      }
    }
    
    // Do we have to change the ISA mode? (ARM-32 only)
    IF_ARM
    (
      if(last_mode != instruction->isa_mode)
      {
        m_callbacks->changeISA((threadid_t)instruction->threadid, instruction->isa_mode);
        last_mode = instruction->isa_mode;
      }
    )
    
    //TODO: NEETHUM
    m_callbacks->sendInstruction((threadid_t)instruction->threadid, (addr_t)instruction->pc, instruction->isize,
                                                instruction->num_addresses, instruction->is_branch, 
                                                taken, instruction->is_predicate, 
                                                instruction->is_executing, instruction->is_before, instruction->is_pause);
    instruction->ndynaddr = 0;
  }
  BUF_PTR(data->seg_base) = data->inst_buf;

}

void DRFrontend::clean_call(void)
{
  void *drcontext = dr_get_current_drcontext();
  process_instructions_buffer(drcontext);
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
  dr_mcontext_t mc = {sizeof(mc),DR_MC_ALL,};
  dr_get_mcontext(drcontext, &mc);
  IF_X86
  (
    // Invoke the handleMagic callback with command (RAX) and arguments (RBX, RCX)
    m_callbacks->handleMagic(map_threadids[dr_get_thread_id(drcontext)], 
                           reg_get_value(DR_REG_RAX, &mc), 
                           reg_get_value(DR_REG_RBX, &mc), 
                           reg_get_value(DR_REG_RCX, &mc) );
  )
  IF_AARCH64
  (
    // Invoke the handleMagic callback with command (X1) and arguments (X2, X3)
    m_callbacks->handleMagic(map_threadids[dr_get_thread_id(drcontext)], 
                           reg_get_value(DR_REG_X1, &mc), 
                           reg_get_value(DR_REG_X2, &mc), 
                           reg_get_value(DR_REG_X3, &mc) );
  )
}

void DRFrontend::invoke_endROI()
{
  void * drcontext = dr_get_current_drcontext();
  m_control->endROI(map_threadids[dr_get_thread_id(drcontext)]);
}

// instrumentation insertion -- after analysis
dr_emit_flags_t DRFrontend::event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
                      bool for_trace, bool translating, void *user_data)
{
    // ignore tool-inserted instrumentation
    if (!instr_is_app(instr))
      return DR_EMIT_DEFAULT;
      
    // Check Magic instructions
    IF_X86
    (
      if( instr_get_opcode(instr) == OP_xchg &&
          instr_num_srcs(instr) == 2 && instr_num_dsts(instr) == 2 &&
          opnd_is_reg(instr_get_src(instr, 0)) && 
          opnd_get_reg(instr_get_src(instr, 0)) == DR_REG_BX && 
          opnd_is_reg(instr_get_dst(instr, 0)) && 
          opnd_get_reg(instr_get_dst(instr, 0)) == DR_REG_BX )
      {
        // Insert clean call to handle magic instructions
        dr_insert_clean_call( drcontext, bb, instr,
                              (void *)magic_clean_call, false, 0);
      }
    )
    IF_AARCH64
    (
      if(instr_get_opcode(instr) == OP_bfm && 
         instr_reg_in_src(instr, DR_REG_X0) && 
         instr_reg_in_dst(instr, DR_REG_X0) &&
         instr_num_srcs(instr) == 4 &&
         opnd_get_immed_int(instr_get_src(instr, 2)) == 0 &&
         opnd_get_immed_int(instr_get_src(instr, 3)) == 0 )
      {
        // Insert clean call to handle magic instructions
        dr_insert_clean_call( drcontext, bb, instr,
                              (void *)magic_clean_call, false, 0);      }
    )
    
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
      int threadid = map_threadids[dr_get_thread_id(drcontext)];  
      // 2: user_data with the number of instructions has been updated in event_bb_analysis
      num_instrs = (uint)(ptr_uint_t)user_data;

      // Insert clean call to count instructions
      dr_insert_clean_call( drcontext, bb, instrlist_first_app(bb),
                            (void *)m_callbacks->countInsns, // frontend's function to call 
                            false, // save fpstate 
                            2, // number of operands
                            OPND_CREATE_INT32(threadid),
                            OPND_CREATE_INT32(num_instrs));
    }
    else  // detailed simulation: send instructions to Sniper's backend
    {
      // Reserve two scratch registers
      reg_id_t reg_ptr, reg_tmp;
      if (drreg_reserve_register(drcontext, bb, instr, NULL, &reg_ptr) != DRREG_SUCCESS ||
          drreg_reserve_register(drcontext, bb, instr, NULL, &reg_tmp) != DRREG_SUCCESS) 
            DR_ASSERT(false); // cannot recover
      
      // Load the address to the buffer to save the collected information of current instruction
      insert_load_buf_ptr(drcontext, bb, instr, reg_ptr);
      
      // Collect instruction's info to send to the backend using meta instructions
      // 1: current thread's id
      int tid = map_threadids[dr_get_thread_id(drcontext)]; 
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
        map_taken_branch[tid][(ptr_uint_t)address] = (ptr_uint_t)targ;
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
        dr_mcontext_t mcontext = {sizeof(mcontext),DR_MC_ALL,};
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
        dr_insert_clean_call(drcontext, bb, instr, (void *)clean_call, false, 0);
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
  std::cerr << "Main client" << std::endl;
  frontend::ExecFrontend<DRFrontend>(argc, argv).start();
}


