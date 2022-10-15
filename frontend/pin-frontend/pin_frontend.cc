
// Additions to fix issues with PinCRT
extern "C" {
#include "upperlower.h"
}
#define try if (1)
#define catch(...) if (0)

#include "shared_ptr.h"
#include "frontend.h"
#include "globals.h"
#include "frontend_threads.h"
//#include "pin.H"

/**
 * @class PinFrontend
 *
 * Frontend class for the Intel PIN based implementation.
 */
class PinFrontend : public frontend::Frontend<PinFrontend>
{
  public:
  PinFrontend()
  {
  }
  void init();
  void start();

  std::string __deleteme(INS ins);

  private:
  // Methods
  static void __sendInstructionSpecialized(threadid_t threadid, uint32_t num_addresses, bool isbefore);
  void initBaseCB();
  static void traceCallback(TRACE trace, void *v);
  static UINT32 addMemoryModeling(INS ins);
  static VOID insertCall(INS ins, IPOINT ipoint, UINT32 num_addresses, BOOL is_branch, BOOL taken);

};


// PinPlay w/ address translation: Translate IARG_MEMORYOP_EA to actual address
ADDRINT translateAddress(ADDRINT addr, ADDRINT size)
{
  MEMORY_ADDR_TRANS_CALLBACK memory_trans_callback = 0;

  // Get memory translation callback if exists
  memory_trans_callback = PIN_GetMemoryAddressTransFunction();

  // Check if we have a callback
  if (memory_trans_callback != 0) {
    // Prepare callback structure
    PIN_MEM_TRANS_INFO mem_trans_info;
    mem_trans_info.addr = addr;
    mem_trans_info.bytes = size;
    mem_trans_info.ip = 0; // ip;
    mem_trans_info.memOpType = PIN_MEMOP_LOAD;
    mem_trans_info.threadIndex = 0; // threadid;

    // Get translated address from callback directly
    addr = memory_trans_callback(&mem_trans_info, 0);
  }

  return addr;
}




namespace frontend
{
  
/**
* @class FrontendOptions<PinFrontend>
*
* Template specialization of FrontedOptions for the command line options with Pin.
* Common members from OptionsBase class are accessible.
* Members specific to Pin frontend are defined here.
*/

template <> class FrontendOptions<PinFrontend> : public OptionsBase<PinFrontend>
{
  public:
  /// Constructor
  /// Parses the command line options, saved in the fields here
  /// Saves status of parsing in m_success
  FrontendOptions(int argc, const char* argv[]);

  /// Destructor
  ~FrontendOptions();

  /// Return a string with the available command line options
  std::string cmd_summary();

  private:
  /// Memory lock to create a new thread within the frontend
  PIN_LOCK new_threadid_lock;

  /// Structure that keeps thread IDs from emulated clone syscalls
  /// that are later used in the frontend thread creation
  //std::deque<ADDRINT> tidptrs;
};

FrontendOptions<PinFrontend>::FrontendOptions(int argc, const char* argv[])
{
  this->parsing_error = true;
  this->current_mode = Sift::ModeIcount;
//  this->any_thread_in_detail = false;
  // FIXME
  //this->any_thread_in_detail = true;  // TODO this was false ^
  this->parsing_error = PIN_Init(argc, const_cast<char**>(argv));
  if (this->parsing_error)
  {
      std::cerr << "Error, invalid parameters" << std::endl;
      std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;
      exit(1);
  }
  PIN_InitSymbols();
  this->verbose = KnobVerbose;
  this->use_roi = KnobUseROI;
  this->fast_forward_target = KnobFastForwardTarget;
  detailed_target = KnobDetailedTarget;
  blocksize = KnobBlocksize;
  output_file = KnobOutputFile;
  emulate_syscalls = KnobEmulateSyscalls;
  response_files = KnobUseResponseFiles;
  send_physical_address = KnobSendPhysicalAddresses;
  stop_address = KnobStopAddress;
  app_id = KnobSiftAppId;
}

inline std::string FrontendOptions<PinFrontend>::cmd_summary()
{
  return KNOB_BASE::StringKnobSummary();
}


/**
 * @class FECopy
 * @brief 
 * Specialized memcopy for Pin
 */

template <> class FECopy<PinFrontend>
{
  public:
    FECopy();
    ~FECopy();
    void copy_to_memory(uint8_t* data_buffer, uint64_t d_addr, uint32_t data_size);
    void copy_from_memory(uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size);
    
};

inline FECopy<PinFrontend>::FECopy()
{
}

inline FECopy<PinFrontend>::~FECopy()
{
}

inline void FECopy<PinFrontend>::copy_to_memory(uint8_t* data_buffer, uint64_t d_addr, uint32_t data_size)
{
  PIN_SafeCopy(reinterpret_cast<void*>(d_addr), data_buffer, data_size);
}

inline void FECopy<PinFrontend>::copy_from_memory(uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
{
  PIN_SafeCopy(data_buffer, reinterpret_cast<void*>(d_addr), data_size);
}
  
/// Specialization of functions

template <> void ExecFrontend<PinFrontend>::handle_frontend_init()
{
  m_frontend->init();
}

template <> void ExecFrontend<PinFrontend>::handle_frontend_start()
{
  m_frontend->start();
}

template <> void ExecFrontend<PinFrontend>::handle_frontend_fini()
{
  PIN_AddFiniFunction((m_frontend->get_control()->Fini), 0);  
}

template <> void FrontendControl<PinFrontend>::removeInstrumentation()
{
  PIN_RemoveInstrumentation();
}

template <> void FrontendControl<PinFrontend>::getCode(uint8_t* dst, const uint8_t* src, uint32_t size)
{
  PIN_SafeCopy(dst, (void*)translateAddress(ADDRINT(src), size), size);
}


// Syscall specialization
template <> class FrontendSyscallModel<PinFrontend> : public FrontendSyscallModelBase<PinFrontend>
{
  // To be able to use the constructors with arguments of the superclass - C++'11 syntax
  using FrontendSyscallModelBase<PinFrontend>::FrontendSyscallModelBase;

  public:
    void initSyscallModeling();
    static void emulateSyscallFunc(threadid_t threadid, CONTEXT *ctxt);
    static void syscallExitCallback(threadid_t threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, VOID *v);
    static void syscallEntryCallback(threadid_t threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, void *v);
};

// template specialization of the Syscall callback
void FrontendSyscallModel<PinFrontend>::emulateSyscallFunc(threadid_t threadid, CONTEXT *ctxt)
{
  // 1: Send a thread ID to the backend if not done yet
  setTID(threadid);
  
  // 2: Collecting frontend-dependent syscall number and args 
  addr_t syscall_number = PIN_GetContextReg(ctxt, REG_GAX);
  sift_assert(syscall_number < MAX_NUM_SYSCALLS);

  syscall_args_t args;
  #if defined(TARGET_IA32)
    args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GBX);
    args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GCX);
    args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
    args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
    args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
    args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GBP);
  #elif defined(TARGET_INTEL64) || defined(TARGET_IA32E)
    args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
    args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
    args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
    args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R10);
    args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R8);
    args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R9);
  #else
    #error "Unknown target architecture, require either TARGET_IA32 or TARGET_INTEL64 | TARGET_IA32E"
  #endif
  
  // 3: Process the syscall and send to the backend
  doSyscall(threadid, syscall_number, args);

}

void FrontendSyscallModel<PinFrontend>::syscallEntryCallback
  (threadid_t threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, void *v)
{
   if (!m_thread_data[threadid].last_syscall_emulated)
   {
      return;
   }

   PIN_SetSyscallNumber(ctxt, syscall_standard, SYS_getpid);
}

void FrontendSyscallModel<PinFrontend>::syscallExitCallback
  (threadid_t threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, VOID *v)
{
   if (!m_thread_data[threadid].last_syscall_emulated)
   {
      return;
   }

   PIN_SetContextReg(ctxt, REG_GAX, m_thread_data[threadid].last_syscall_returnval);
   m_thread_data[threadid].last_syscall_emulated = false;
}

void FrontendSyscallModel<PinFrontend>::initSyscallModeling()
{
   PIN_AddSyscallEntryFunction(syscallEntryCallback, 0);
   PIN_AddSyscallExitFunction(syscallExitCallback, 0);
}


//////////////////////////////////
 
static void handleThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
  FrontendThreads<PinFrontend>::threadStart(threadid);
}

static void handleThreadFinish(THREADID threadid, const CONTEXT *ctxt, INT32 flags, VOID *v)
{
  FrontendThreads<PinFrontend>::threadFinish(threadid, flags);
}

template <> void FrontendThreads<PinFrontend>::callFinishHelper(threadid_t threadid)
{
  PIN_SpawnInternalThread(threadFinishHelper, (void*)(unsigned long)threadid, 0, NULL);
}

template <> void FrontendThreads<PinFrontend>::initThreads()
{
   PIN_AddThreadStartFunction(handleThreadStart, 0);
   PIN_AddThreadFiniFunction(handleThreadFinish, 0);
}

/**
 * @class FELock<PinFrontend>
 * @brief 
 * Lock specialized for Pin
 */

template <> class FELock<PinFrontend> : public LockBase<PinFrontend>
{
  public:
    FELock();
    ~FELock();
    void acquire_lock(threadid_t tid);
    void release_lock();
  private:
    PIN_LOCK the_lock;
    
};

inline FELock<PinFrontend>::FELock()
{
  PIN_InitLock(&(this->the_lock));
}

inline FELock<PinFrontend>::~FELock()
{
}

inline void FELock<PinFrontend>::acquire_lock(threadid_t tid)
{
  PIN_GetLock(&(this->the_lock), tid);
}

inline void FELock<PinFrontend>::release_lock()
{ 
  PIN_ReleaseLock(&(this->the_lock));
}

} // namespace frontend

// Code specific to Pin called inside the general sendInstruction function
void PinFrontend::__sendInstructionSpecialized(threadid_t threadid, uint32_t num_addresses, bool isbefore)
{
  if (isbefore)
  {
    for(uint8_t i = 0; i < num_addresses; ++i)
    {
      // If the instruction hasn't executed yet, access the address to ensure a page fault if the mapping wasn't set up yet
      static char dummy = 0;
      dummy += *(char *)translateAddress(m_thread_data[threadid].dyn_addresses[i], 0);
    }
  }
}

VOID PinFrontend::insertCall(INS ins, IPOINT ipoint, UINT32 num_addresses, BOOL is_branch, BOOL taken)
{
   INS_InsertCall(ins, ipoint,
      AFUNPTR(m_callbacks->sendInstruction),
      IARG_THREAD_ID,
      IARG_ADDRINT, INS_Address(ins),
      IARG_UINT32, UINT32(INS_Size(ins)),
      IARG_UINT32, num_addresses,
      IARG_BOOL, is_branch,
      IARG_BOOL, taken,
      IARG_BOOL, INS_IsPredicated(ins),
      IARG_EXECUTING,
      IARG_BOOL, ipoint == IPOINT_BEFORE,
      IARG_BOOL, INS_Opcode(ins) == XED_ICLASS_PAUSE,
      IARG_END);
}

UINT32 PinFrontend::addMemoryModeling(INS ins)
{
   UINT32 num_addresses = 0;

   if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
   {
      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         INS_InsertCall(ins, IPOINT_BEFORE,
               AFUNPTR(handleMemory),
               IARG_THREAD_ID,
               IARG_MEMORYOP_EA, i,
               IARG_END);
         num_addresses++;
      }
   }
   sift_assert(num_addresses <= Sift::MAX_DYNAMIC_ADDRESSES);

   return num_addresses;
}

void PinFrontend::traceCallback(TRACE trace, void *v)
{
  BBL bbl_head = TRACE_BblHead(trace);

  for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
  {
//// TODO first loop with handle magic, end address

    for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
    {
      // Handle emulated syscalls
      if (INS_IsSyscall(ins))
      {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(m_sysmodel->emulateSyscallFunc), IARG_THREAD_ID, IARG_CONST_CONTEXT, IARG_END);
      }

      if (ins == BBL_InsTail(bbl))
        break;
    }

// TODO not thread in detail
//    if (current_mode == Sift::ModeDetailed)
//    {
      for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
      {
        // For memory instructions, collect all addresses at IPOINT_BEFORE
        UINT32 num_addresses = addMemoryModeling(ins);

       bool is_branch = INS_IsBranch(ins) && INS_HasFallThrough(ins);

        if (is_branch)
        {
          insertCall(ins, IPOINT_AFTER,        num_addresses, true  /* is_branch */, false /* taken */);
          insertCall(ins, IPOINT_TAKEN_BRANCH, num_addresses, true  /* is_branch */, true  /* taken */);
        }
        else
        {
          // Whenever possible, use IPOINT_AFTER as this allows us to process addresses after the application has used them.
          // This ensures that their logical to physical mapping has been set up.
          insertCall(ins, INS_HasFallThrough(ins) ? IPOINT_AFTER : IPOINT_BEFORE, num_addresses, false /* is_branch */, false /* taken */);
        }

        if (ins == BBL_InsTail(bbl))
          break;
      }
//    }
// TODO mode memory
   }
}


void PinFrontend::initBaseCB()
{
   TRACE_AddInstrumentFunction(traceCallback, 0);
}

void PinFrontend::init()
{
  this->num_threads = 0;
  if (KnobEmulateSyscalls.Value()) //TODO change this knob to own variable
  {
    if (!KnobUseResponseFiles.Value())
    {
      std::cerr << "Error, Response files are required when using syscall emulation." << std::endl;
      exit(1);
    }

    m_sysmodel->initSyscallModeling();
  }
  // Init callbacks
  initBaseCB();
  // Thread-related callbacks
  m_threads->initThreads();
}

void PinFrontend::start()
{
  PIN_StartProgram();
}


// To avoid error loading pin; remove once it is actually used by the trace routine function
std::string PinFrontend::__deleteme(INS ins)
{
  ADDRINT eip = INS_Address(ins);
  INT32 column = 0, line = 0;
  std::string filename = "??";
  PIN_GetSourceLocation(eip, &column, &line, &filename);
  return filename;
}


// START_FRONTEND_EXEC(PinFrontend);

int main(int argc, const char* argv[])
{
  frontend::ExecFrontend<PinFrontend>(argc, argv).start();
}

