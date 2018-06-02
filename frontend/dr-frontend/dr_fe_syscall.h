#ifndef _DR_FE_SYSCALL_H_
#define _DR_FE_SYSCALL_H_

namespace frontend
{
  
template <> class FrontendSyscallModel<DRFrontend> : public FrontendSyscallModelBase<DRFrontend>
{
  // To be able to use the constructors with arguments of the superclass - C++'11 syntax
  using FrontendSyscallModelBase<DRFrontend>::FrontendSyscallModelBase;

  public:
    void initSyscallModeling();
    static bool event_pre_syscall(void *drcontext, int sysnum);
    static bool event_filter_syscall(void *drcontext, int sysnum);
    void set_map_threads(std::unordered_map<int, threadid_t>* mt){ this->map_threadids = mt; }

  private:
    static const int MAX_SYSCALL_ARGS = 6;  // platform dependent, this is for Linux
    static std::unordered_map<int, threadid_t>* map_threadids;  // needed to know with which Sniper thread we're working
};

} // end namespace frontend

#include "dr_fe_syscall.tcc"

#endif // _DR_FE_SYSCALL_H_