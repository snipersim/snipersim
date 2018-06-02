#include <iostream>
#include "../include/sim_api.h"

namespace frontend
{

template <typename T>
thread_data_t* FrontendCallbacks<T>::m_thread_data;
template <typename T>
FrontendOptions<T>* FrontendCallbacks<T>::m_options;
template <typename T>
FrontendControl<T>* FrontendCallbacks<T>::m_control;

template <typename T>
void FrontendCallbacks <T>::countInsns(threadid_t threadid, int32_t count)
{
   m_thread_data[threadid].icount += count;

   if (!m_control->get_any_thread_in_detail() && m_thread_data[threadid].output)
   {
      m_thread_data[threadid].icount_reported += count;
      if (m_thread_data[threadid].icount_reported > m_options->get_flow_control_ff())
      {
         Sift::Mode mode = m_thread_data[threadid].output->InstructionCount(m_thread_data[threadid].icount_reported);
         m_thread_data[threadid].icount_reported = 0;
         m_control->setInstrumentationMode(mode);
      }
   }

   if (m_thread_data[threadid].icount >= m_options->get_fast_forward_target() 
       && !m_control->get_in_roi() && !m_options->get_use_roi() && !m_options->get_mpi_implicit_roi())
   {
      if (m_options->get_verbose())
      {
         std::cerr << "[SIFT_RECORDER:" << m_options->get_app_id() << ":" << m_thread_data[threadid].thread_num << 
            "] Changing to detailed after " << m_thread_data[threadid].icount << " instructions" << std::endl;
      }
      if (!m_thread_data[threadid].output)
         m_control->openFile(threadid);
      m_thread_data[threadid].icount = 0;
      m_control->set_in_roi(true);
      m_control->setInstrumentationMode(Sift::ModeDetailed);
   }
}

template <typename T>
void FrontendCallbacks <T>::sendInstruction(threadid_t threadid, addr_t addr, uint32_t size, uint32_t num_addresses, bool is_branch, bool taken, bool is_predicate, bool executing, bool isbefore, bool ispause)
{
  // We're still called for instructions in the same basic block as ROI end, ignore these
  if (!m_thread_data[threadid].output)
  {
    std::cerr << "Output still not open for Threadid:" << threadid << std::endl;
    m_thread_data[threadid].num_dyn_addresses = 0;
    return;
  }
  //std::cerr << "Threadid:" << threadid << std::endl;

  ++m_thread_data[threadid].icount;
  ++m_thread_data[threadid].icount_detailed;

  // Reconstruct basic blocks (we could ask Pin, but do it the same way as TraceThread will do it)
  if (m_thread_data[threadid].bbv_end || m_thread_data[threadid].bbv_last != addr)
  {
    // We're the start of a new basic block
    m_thread_data[threadid].bbv->count(m_thread_data[threadid].bbv_base, m_thread_data[threadid].bbv_count);
    m_thread_data[threadid].bbv_base = addr;
    m_thread_data[threadid].bbv_count = 0;
  }
  m_thread_data[threadid].bbv_count++;
  m_thread_data[threadid].bbv_last = addr + size;
  // Force BBV end on non-taken branches
  m_thread_data[threadid].bbv_end = is_branch;

  /*std::cerr << "[FRONTEND:"<<threadid<<"]  "<< std::hex << addr<< " with mem addresses: " ;
  for(int i = 0; i<num_addresses; i++)
    std::cerr << m_thread_data[threadid].dyn_addresses[i] << " ";
  std::cerr << std::endl;*/

  sift_assert(m_thread_data[threadid].num_dyn_addresses == num_addresses);
  /*if(m_thread_data[threadid].num_dyn_addresses != num_addresses){
    std::cerr << "[FRONTEND:"<<threadid<<"]  ERROR with instruction "<< std::hex << addr<< "; num addresses: " 
    << num_addresses << "; num dyn addresses: " << m_thread_data[threadid].num_dyn_addresses << std::endl;
  }*/
  
  FrontendCallbacks<T>::__sendInstructionSpecialized(threadid, num_addresses, isbefore);
  m_thread_data[threadid].output->Instruction(addr, size, num_addresses, m_thread_data[threadid].dyn_addresses, is_branch, taken, is_predicate, executing);
  //std::cerr << threadid << "  Instruction sent" << std::endl;
  m_thread_data[threadid].num_dyn_addresses = 0;
    
  // TODO fixme with response files
  if (m_options->get_response_files() && m_options->get_flow_control() 
      && (m_thread_data[threadid].icount > m_thread_data[threadid].flowcontrol_target || ispause))
  {
    // std::cerr << "Going to sync" << std::endl;

    Sift::Mode mode = m_thread_data[threadid].output->Sync();
    m_thread_data[threadid].flowcontrol_target = m_thread_data[threadid].icount + 1000;//KnobFlowControl.Value();
    m_control->setInstrumentationMode(mode);
  }
   // std::cerr << "Icount: " << m_thread_data[threadid].icount << std::endl;


  // TODO fill in cases with use response files, detailed target, blocksize
}

template <typename T>
void FrontendCallbacks <T>::changeISA(threadid_t threadid, int new_isa)
{
  // We're still called for instructions in the same basic block as ROI end, ignore these
  if (!m_thread_data[threadid].output)
  {
    std::cerr << "Output still not open for Threadid:" << threadid << std::endl;
    m_thread_data[threadid].num_dyn_addresses = 0;
    return;
  }
  
  m_thread_data[threadid].output->ISAChange(new_isa);
}

template <typename T>
addr_t FrontendCallbacks <T>::handleMagic(threadid_t threadid, addr_t reg_0, addr_t reg_1, addr_t reg_2)
{
   uint64_t res = reg_0; 

   if (m_options->get_response_files() && m_thread_data[threadid].running && m_thread_data[threadid].output)
   {
      res = m_thread_data[threadid].output->Magic(reg_0, reg_1, reg_2);
   }

   if (reg_0 == SIM_CMD_ROI_START)
   {
      if (m_options->get_use_roi() && !m_control->get_in_roi())
        m_control->beginROI(threadid);
   }
   else if (reg_0 == SIM_CMD_ROI_END)
   {
      if (m_options->get_use_roi() && m_control->get_in_roi())
         m_control->endROI(threadid);
   }

   return res;
}


} // namespace frontend
