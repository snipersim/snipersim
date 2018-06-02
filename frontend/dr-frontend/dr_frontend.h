#ifndef _DR_FRONTEND_H_
#define _DR_FRONTEND_H_

#include "frontend.h"

#include "dr_api.h"

#include <unordered_map>

#include "frontend_threads.h"

#include "dr_fe_macros.h"

/**
 * @class DRFrontend
 *
 * Frontend class for the DynamoRIO based implementation.
 */
class DRFrontend : public frontend::Frontend<DRFrontend>
{
  public:
    DRFrontend() {}  // ctor
    ~DRFrontend();  // dtor
    
    // Specialization of methods
    /// Frontend initialization, called from the specialization of ExecFrontend::handle_frontend_init().
    /// Initializes DynamoRio scratch registers, system call emulation and instrumentation callbacks.
    /// Reserves local storage for each application thread.
    void init();
    
    /// Legacy method for starting the frontend; relevant for Pin but not for DynamoRIO.
    void start();
    
    /// Allocation of the struct that holds the SIFT threads information.
    /// This specialized method is call at the initialization of the FrontendExec module.
    void allocate_thread_data(size_t thread_data_size);
    
    // DR callbacks and support functions
    /// DR callback invoked when a new thread is created in the target application.
    /// Allocates thread-private storage to save the information that will be processed in process_instructions_buffer().
    static void event_thread_init(void *drcontext);
    
    /// DR callback invoked when a thread is destroyed, freeing private storage.
    static void event_thread_exit(void *drcontext);
    
    /// DR callback invoked at the end, to clean up space and registered callbacks.
    static void event_exit();
    
    /// DR clean call to process and send instructions from a buffer to the backend.
    static void process_instructions_buffer(void *drcontext);
    static void clean_call(void);
    
    /// DR clean call to process magic instructions.
    static void magic_clean_call();
    
    /// Specialization of the method to invoke the end of a Region of Interest if the end address has been reached.
    static void invoke_endROI();
    
    /// Method to obtain memory accesses information from the execution in the frontend to let Sniper model memory at the backend side.
    static unsigned int addMemoryModeling(threadid_t threadid, void *drcontext, instrlist_t *bb, instr_t *instr, 
                                          reg_id_t base, reg_id_t scratch);
    static void handleMemory(threadid_t threadid, void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t base, 
                             reg_id_t scratch, ptr_uint_t address, int disp_value_dynaddr);
    
    /// Methods to insert meta instructions to collect information in runtime.
    static void insert_save_pc(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t base, reg_id_t scratch, app_pc pc);
    static void insert_save_int(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t base, reg_id_t scratch, int disp, int tid);
    static void insert_load_buf_ptr(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t reg_ptr);
    static void insert_update_buf_ptr(void *drcontext, instrlist_t *bb, instr_t *instr, reg_id_t reg_ptr, int adjust);

  private:
    /// Struct that contains the instruction information
    typedef struct {
      int threadid;
      app_pc pc;
      int isize;
      int num_addresses;
      bool is_branch;
#if defined(AARCH64)  // This is to avoid DynamoRIO crashing on ARM64 -- fields have to be aligned.
      bool __is_branch;
#endif
      bool is_predicate;
#if defined(AARCH64)
      bool __is_predicate;
#endif
      bool is_executing;
#if defined(AARCH64)
      bool __is_executing;
#endif
      bool is_pause;
#if defined(AARCH64)
      bool __is_pause;
#endif
      bool is_before;
#if defined(AARCH64)
      bool __is_before;
#endif
      // buffering also this information that will update original one in thread_data
      // app_pc value_dynaddr[Sift::MAX_DYNAMIC_ADDRESSES];  // Changed to different variables to be able to use offsetof
      app_pc dynaddr_0;
      app_pc dynaddr_1;
      app_pc dynaddr_2;
      unsigned int ndynaddr;
      // isa mode to tell Sniper if the instruction is ARM or Thumb ISA
      int isa_mode;
    }  instruction_t;  
    /// Struct that holds the private information collected by each thread to sent to Sniper's backend
    typedef struct {
      byte      *seg_base;
      instruction_t *inst_buf;
    } per_thread_t;
    /// Allocated TLS slot offsets
    enum {
      FRONTEND_TLS_OFFS_BUF_PTR,
      FRONTEND_TLS_COUNT, // total number of TLS slots allocated 
    };
    // Methods
    /// Instrumentation function to be inserted in DR transformation time (native code -> IR).
    /// It passes the number of instructions in the current BB collected by event_bb_analysis and the current threadid 
    /// to the countInstructions function.
    static dr_emit_flags_t event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
                      bool for_trace, bool translating, void *user_data);
                      
    /// Instrumentation function to be inserted in transformation time to count the number of instructions in the
    /// current basic block (instrlist_t *bb), saved in user_data.
    static dr_emit_flags_t event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                  bool for_trace, bool translating, void **user_data);
  // Variables
    /// Correspondance DR thread id / Frontend thread Id
    static std::unordered_map<int, threadid_t> map_threadids;
    /// Maps with the address of a branch instruction and its taken target addresses.
    /// Each thread has its own map, which is accessed with the Frontend thread Id.
    static std::vector<std::unordered_map<ptr_uint_t,  ptr_uint_t>> map_taken_branch;
    /// Number threads created so far
    static threadid_t next_threadid;
    /// Start address of the application module
    static app_pc exe_start;
    /// Pointer to the linear address of the TLS base, can be obtained with dr_get_dr_segment_base 
    static reg_id_t tls_seg;
    /// Offset to access the TLS
    static uint tls_offs;
    /// Index of current thread local storage
    static int tls_idx;
    /// Keeps the last address of a memory operand seen
    static app_pc last_opnd;
    /// Keeps the mode of the last instruction of the previously processed buffer
    static int last_mode;
};

#endif // _DR_FRONTEND_H_