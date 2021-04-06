#include <algorithm>
#include <qsim.h>
#include <qsim-load.h>
#include <qsim-regs.h>
#include <capstone.h>
#include "distorm.h"
#include "frontend.h"

/**
 * @class QsimFrontend
 *
 * Frontend class for the QSim based implementation.
 */
class QsimFrontend : public frontend::Frontend<QsimFrontend>
{
  public:
  QsimFrontend(FrontendISA theISA);
  void init(int ncores, std::string statefile);
  void start(int ncores, std::string app);
  int app_start_cb(int c);
  int app_end_cb(int c);
  void inst_cb(int cpu, uint64_t vaddr, uint64_t paddr, uint8_t len, const uint8_t *bytes, enum inst_type t);
  void mem_cb(int cpu, uint64_t vaddr, uint64_t paddr, uint8_t size, int is_write);
  void reg_cb(int cpu, int reg, uint8_t size, int rtype);
  void set_fini_func(void *f, void *o);

  private:
    class DecodedInstruction
    {
      public:
        virtual void do_decode(unsigned char * code, size_t size, uint64_t address) = 0;
        std::string operands;
        std::string mnemonic;
    };
    class X86DecodedInst : public DecodedInstruction
    {
      public:
        X86DecodedInst() {}
        virtual void do_decode(unsigned char * code, size_t size, uint64_t address) override {
          distorm_decode(0, code, size, Decode64Bits, decoinst, 15, &shouldBeOne);
          mnemonic = reinterpret_cast<char*>(decoinst[0].mnemonic.p);
          operands = reinterpret_cast<char*>(decoinst[0].operands.p);
        }
        private:
          _DecodedInst decoinst[15];  // decode result is written here
          unsigned int shouldBeOne;  // number of instructions succesfully disassembled
    };
    class ARMDecodedInst : public DecodedInstruction
    {
      public:
        ARMDecodedInst() {    
          cs_err err = cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle);
          if (err) {
            std::cerr << "Failed on cs_open with error: " << err << std::endl;
            return;
          }
          cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
        }
        virtual void do_decode(unsigned char * code, size_t size, uint64_t address) override {
          size_t count;
          count = cs_disasm(handle, code, size, address, 0, &insn);
          mnemonic = insn[0].mnemonic;
          operands = insn[0].op_str;
          cs_free(insn, count);
        }
      private:
        csh handle;  // FIXME: maybe it has to be declared static
        cs_insn *insn; 
    };
  
    class QsimInstruction
    {
      public:
        // members
        int threadid;  // Sniper's thread ID
        int cpu;  // Qsim's CPU ID
        enum inst_type itype;
        uint64_t vaddr;
        uint64_t paddr;
        uint8_t ilen;
        uint32_t num_addresses;
        bool is_branch;
        bool taken;
        bool is_predicate;
        bool executing;
        bool isbefore;
        bool ispause;
        DecodedInstruction *dinst;
        std::vector<int> regs;
        std::vector<int> regtypes;
    };
    
    Qsim::OSDomain* osd_p;
    bool finished;
    void *fini_func;
  
    // Correspondance Qsim CPU / Frontend thread id
    // As CPU IDs in Qsim are completely random, each CPU in Qsim may correspond to a different threadid in Frontend
    std::vector<uint64_t> cpus;
    
    // last Frontend thread id assigned
    int last_threadid;
  
    // Current instrumented instruction
    QsimInstruction cur_inst;
    
    static void __sendInstructionSpecialized(threadid_t threadid, uint32_t num_addresses, bool isbefore);
};


namespace frontend
{
  
// Syscall specialization
template <> class FrontendSyscallModel<QsimFrontend> : public FrontendSyscallModelBase<QsimFrontend>
{
  // To be able to use the constructors with arguments of the superclass - C++'11 syntax
  using FrontendSyscallModelBase<QsimFrontend>::FrontendSyscallModelBase;

  public:
    // Before calling this function, osd_p has to be set 
    static void emulateSyscallFunc(threadid_t threadid, int qsim_cpu, addr_t syscall_number);
    Qsim::OSDomain* get_osd();
    void set_osd(Qsim::OSDomain* p);
    static Qsim::OSDomain* osd_p;
};

Qsim::OSDomain* FrontendSyscallModel<QsimFrontend>::osd_p;

inline
Qsim::OSDomain* FrontendSyscallModel<QsimFrontend>::get_osd() 
{
  return osd_p;
}

inline
void FrontendSyscallModel<QsimFrontend>::set_osd(Qsim::OSDomain* p)
{
  osd_p = p;
}

// template specialization of the Syscall callback
void FrontendSyscallModel<QsimFrontend>::emulateSyscallFunc(threadid_t threadid, 
                                                            int qsim_cpu, addr_t syscall_number)
{
  std::cerr << "[FRONTEND:"<<threadid<<"] emulateSyscallFunc: entry" << std::endl;
  // 1: Send a thread ID to the backend if not done yet
  setTID(threadid);
  
  // 2: Collecting frontend-dependent syscall args 
  syscall_args_t args;
  args[0] = osd_p->get_reg(qsim_cpu, QSIM_X86_RDI);
  args[1] = osd_p->get_reg(qsim_cpu, QSIM_X86_RSI);
  args[2] = osd_p->get_reg(qsim_cpu, QSIM_X86_RDX);
  args[3] = osd_p->get_reg(qsim_cpu, QSIM_X86_R10);
  args[4] = osd_p->get_reg(qsim_cpu, QSIM_X86_R8);
  args[5] = osd_p->get_reg(qsim_cpu, QSIM_X86_R9);
  
  // 3: Process the syscall and send to the backend
  doSyscall(threadid, syscall_number, args);
  std::cerr << "[FRONTEND:"<<threadid<<"] emulateSyscallFunc: back from doing syscall" << std::endl;

}


template<>
void Frontend<QsimFrontend>::__sendInstructionSpecialized(threadid_t threadid, uint32_t num_addresses, bool isbefore)
{
}


} // namespace frontend

QsimFrontend::QsimFrontend(FrontendISA theISA) 
: finished(false)
{
  switch(theISA)
  {
    case ARM_AARCH64:
      cur_inst.dinst = new ARMDecodedInst();
      break;
    case INTEL_X86_64:
      cur_inst.dinst = new X86DecodedInst();
      break;
    default:
      std::cerr << "ISA not supported by QSim" << std::endl;
  }
}

void QsimFrontend::init(int ncores, std::string statefile)
{
  // Initialize the mapping vector v[qsim_cpu] = tid
  // -1: no tid assigned to that cpu yet
  this->cpus.assign(ncores, -1);

  //std::cout << "[DEBUG] Number of cores = " << ncores << std::endl;

  // Create a new OSDomain from a previously saved state.
  osd_p = new Qsim::OSDomain(ncores, statefile.c_str());
  //std::cout << "[DEBUG] Success creating OSDomain" << std::endl;
  this->m_sysmodel->set_osd(osd_p);

  // Setup callbacks
  osd_p->set_app_start_cb(this, &QsimFrontend::app_start_cb);
  //std::cout << "[DEBUG] Success setting callbacks" << std::endl;
}

void QsimFrontend::start(int ncores, std::string app)
{
  //std::cout << "[DEBUG] Going to load application" << std::endl;

  // Load application
  Qsim::load_file(*osd_p, app.c_str());
  //std::cout << "[DEBUG] Success loading file" << std::endl;

  // Main emulation loop: run until 'finished' is true.
  while (!finished) {
    for (int i = 0; i < ncores; i++)
      osd_p->run(i, 1); // TODO instructions_per_iteration = 1000 ??
    osd_p->timer_interrupt();
    //std::cout << "[DEBUG] In QSim loop" << std::endl;
  }

  delete osd_p; // TODO make osd_p a member, to delete it when calling to remove instrumentation
}

int QsimFrontend::app_start_cb(int c)
{     
  // Set instrumentation
  osd_p->set_inst_cb(this, &QsimFrontend::inst_cb);
  // osd.set_atomic_cb(this, &QsimFrontend::atomic_cb);
  osd_p->set_mem_cb(this, &QsimFrontend::mem_cb);
  // osd.set_int_cb(this, &QsimFrontend::int_cb);
  // osd.set_io_cb(this, &QsimFrontend::io_cb);
  osd_p->set_reg_cb(this, &QsimFrontend::reg_cb);
  //std::cout << "Entered start " << std::endl;

  osd_p->set_app_end_cb(this, &QsimFrontend::app_end_cb);

  return 1;
}

void QsimFrontend::inst_cb(int cpu, uint64_t va, uint64_t pa, uint8_t len, const uint8_t *bytes, enum inst_type t)
{
  std::vector<std::string> branch_insts = {"JO", "JNO", "JB", "JAE", "JZ", "JNZ", "JBE", "JA", "JS", "JNS", "JP", "JNP", "JL", "JGE", "JLE", "JG"};
  
  if(this->num_threads == 0)  // First instruction -- initialize for the next execution
  {
    last_threadid = 0;
    this->cpus[cpu] = last_threadid; 
    m_threads->threadStart(last_threadid);
  }
  else //if(cur_inst.vaddr < 0xffffffff00000000)
  {
     
    // Is this instruction part of a system call? Do not send to Sniper
    // System instructions are in the address range of 0xffffffffXXXXXXXX
    /*if(va > 0xffffffff00000000)
    {
      //std::cerr << "[FRONTEND] I'm a system instruction: " << std::hex << va << std::endl;
      return;
    } else {
      std::cerr << "[FRONTEND] I'm a user instruction: " << std::hex << va << std::endl;
    }*/
      
    // Was previous instruction (still in cur_inst) a branch and was taken?
    cur_inst.taken = cur_inst.is_branch && ( va != (cur_inst.vaddr + cur_inst.ilen) );
    
    // Handle special cases
    std::string::size_type start_pos, end_pos;
    std::stringstream strstrm;
    // Memory access not detected by Qsim -- TODO change Qsim to handle this?
    if (cur_inst.itype == QSIM_INST_CALL) {
      //std::cerr << "[SPECIAL] Is a call " << std::endl;
      if (cur_inst.dinst->operands.find("[") != std::string::npos && cur_inst.num_addresses < 2) {
        //std::cerr << "[SPECIAL] Is too few " << std::endl;
        int new_addr = 0;  // TODO FIXME
        cur_inst.num_addresses++;
        handleMemory(cur_inst.threadid, new_addr);
        cur_inst.num_addresses++;
        handleMemory(cur_inst.threadid, new_addr);
      } else if (cur_inst.num_addresses == 0) {
        // Memory access of CALL instruction not registered
        int call_addr;
        cur_inst.num_addresses++;
        strstrm << cur_inst.dinst->operands;
        strstrm >> std::hex >> call_addr;
        handleMemory(cur_inst.threadid, call_addr);
      }
    } else if (cur_inst.num_addresses == 0 && ((start_pos = cur_inst.dinst->operands.find("[")) != std::string::npos)) {
      // extract address
      std::string new_addr_str = "0x0";
      int new_addr;
      start_pos++;
      end_pos = cur_inst.dinst->operands.find("]", start_pos);
      if (end_pos != std::string::npos) {
        //std::cerr << "[GS_SPECIAL] Extracting positions "<< start_pos << " and " << end_pos - start_pos << " from string: " << cur_inst.dinst->operands << std::endl;
        new_addr_str = cur_inst.dinst->operands.substr(start_pos, end_pos - start_pos);
      } 
      cur_inst.num_addresses++;
      strstrm << new_addr_str;
      strstrm >> std::hex >> new_addr;
      //std::cerr << "[GS_SPECIAL] " << std::hex << gs_addr << std::endl;
      handleMemory(cur_inst.threadid, new_addr);
    } else if (cur_inst.num_addresses == 0 && \
                (cur_inst.itype == QSIM_INST_RET || \
                (cur_inst.dinst->mnemonic.find("PUSH") != std::string::npos) || \
                (cur_inst.dinst->mnemonic.find("POP") != std::string::npos) || \
                (cur_inst.dinst->mnemonic.find("LEAVE") != std::string::npos) || \
                (cur_inst.dinst->mnemonic.find("REP") != std::string::npos) || \
                (cur_inst.dinst->mnemonic.find("DB") != std::string::npos))) {
      // Memory access of RET instruction not registered
      int new_addr = 0;  // TODO FIXME
      cur_inst.num_addresses++;
      handleMemory(cur_inst.threadid, new_addr);
      if (cur_inst.dinst->mnemonic.find("REP") != std::string::npos) {
        cur_inst.num_addresses++;
        handleMemory(cur_inst.threadid, new_addr);
      }
    }
    if (t == QSIM_INST_TRAP) {  
      addr_t syscall_number = osd_p->get_reg(cur_inst.cpu, QSIM_X86_RAX);
      std::cerr << "[FRONTEND:"<<cur_inst.threadid<<"] emulateSyscallFunc: syscall number = " << syscall_number << std::endl;
      sift_assert(syscall_number < MAX_NUM_SYSCALLS);

      // Emulate Syscall 
      m_sysmodel->emulateSyscallFunc(cur_inst.threadid, cur_inst.cpu, syscall_number);  
      std::cerr << "Last threadid: " << last_threadid << std::endl;
      
      // Create new frontend thread structure if there is a clone.
      // In other emulators this is done with a callback at the start of a thread, but Qsim does not support that
      uint64_t args0 = osd_p->get_reg(cur_inst.cpu, QSIM_X86_RDI);
      if( (syscall_number == SYS_clone) && (args0 & CLONE_THREAD) )
      {
        this->cpus[cpu] = ++last_threadid; 
        m_threads->threadStart(last_threadid);
      }
    }
    // Send previous instruction
    std::cerr << "["<<cur_inst.threadid<<"] Invoking sendInstruction (type = " << cur_inst.itype << \
    ", branch = " << cur_inst.is_branch << "); Decoded: " << cur_inst.dinst->mnemonic << " " << \
    cur_inst.dinst->operands << " A: " << std::hex << cur_inst.vaddr << " ( " << cur_inst.paddr << " ) " << " with num_addresses: " << cur_inst.num_addresses << std::endl;
    /*
    std::cerr << "[REGs] ";
    for (std::vector<int>::const_iterator i = cur_inst.regs.begin(); i != cur_inst.regs.end(); ++i)
      std::cout << *i << ' ';
    std::cerr << std::endl << "[TYPEs] "; 
    for (std::vector<int>::const_iterator i = cur_inst.regtypes.begin(); i != cur_inst.regtypes.end(); ++i)
      std::cout << *i << ' ';
    std::cout << std::endl;*/
    sendInstruction(cur_inst.threadid, cur_inst.vaddr, cur_inst.ilen, cur_inst.num_addresses, cur_inst.is_branch, cur_inst.taken, cur_inst.is_predicate, cur_inst.executing, cur_inst.isbefore, cur_inst.ispause);
  }  

  // clear and update data of cur_inst for the next sending 
  cur_inst.num_addresses = 0; // updated in mem_cb
  cur_inst.regs.clear();
  cur_inst.regtypes.clear(); 
  cur_inst.threadid = cpus[cpu];  // convert to Frontend's TId
  cur_inst.cpu = cpu;
  cur_inst.itype = t;
  cur_inst.dinst->do_decode((unsigned char *) bytes, len, va);
  // Is this instruction a (conditional) branch?
  if (std::find(branch_insts.begin(), branch_insts.end(), cur_inst.dinst->mnemonic) != branch_insts.end()) {
    cur_inst.is_branch = true;
  } else {
    cur_inst.is_branch = false;
  }

  cur_inst.vaddr = va;
  cur_inst.paddr = pa;
  cur_inst.ilen = len;
  cur_inst.is_predicate = false; // TODO change; apparently there are predicate instructions detected in Intel by Pin??
  cur_inst.executing = true; // TODO change; related to predicate
  cur_inst.isbefore = true;
  cur_inst.ispause = false; // TODO change
}

void QsimFrontend::mem_cb(int cpu, uint64_t vaddr, uint64_t paddr, uint8_t size, int is_write)
{
  cur_inst.num_addresses++;
  // update the memory modeling with the actual TId mapping
  handleMemory(cpus[cpu], paddr);
}

void QsimFrontend::reg_cb(int cpu, int reg, uint8_t size, int rtype)
{
  cur_inst.regs.push_back(reg);
  cur_inst.regtypes.push_back(rtype);
}

int QsimFrontend::app_end_cb(int c)
{
  //std::cout << "Entered finish " << std::endl;
  finished = true;
  
  //call to Fini function
  m_control->Fini(0, NULL);
  
  return 0;
}

// START_FRONTEND_EXEC(QsimFrontend);

int main(int argc, const char* argv[])
{
  frontend::ExecFrontend<QsimFrontend>(argc, argv).start();
}


namespace frontend
{

/// Specialization of functions from the frontend namespace

template <> void ExecFrontend<QsimFrontend>::handle_frontend_init()
{
  m_frontend->init(m_options->get_ncores(), m_options->get_statefile());
}

template <> void ExecFrontend<QsimFrontend>::handle_frontend_start()
{
  m_frontend->start(m_options->get_ncores(), m_options->get_cmd_app());
}

template <> void ExecFrontend<QsimFrontend>::handle_frontend_fini()
{

}

/*   
// Get code for using only virtual instructions and no system ones
template <> void FrontendControl<QsimFrontend>::getCode(uint8_t* dst, const uint8_t* src, uint32_t size)
{
  //std::memcpy( dst, src, size );
  
  uint32_t i;
  //std::cerr << "Getting code of size:" << size << std::endl;
  for (i=0; i<size; i++)
  {
    //std::cerr << "Iteration " << i << std::endl;
    // TODO fixme! cpu number changes from 1 to the current cpu
    dst[i] = m_sysmodel->osd_p->cpus[0]->mem_rd_virt(1, reinterpret_cast<uint64_t>(src+i));
    //std::cerr << std::hex << reinterpret_cast<uint64_t>(src+i) << ": " <<(unsigned)dst[i] << std::endl;
  }
}*/

}