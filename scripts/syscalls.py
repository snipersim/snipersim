import sim

class LogSyscalls:
  def hook_syscall_enter(self, threadid, coreid, time, syscall_number, args):
    print '[SYSCALL] @%d ns: thread(%d) core(%d) syscall(%d) args(%s)' % (time/1e6, threadid, coreid, syscall_number, args)

  def hook_syscall_exit(self, threadid, coreid, time, ret_val, emulated):
    print '[SYSCALL] @%d ns: thread(%d) core(%d) syscall exit ret_val(%d) emulated(%s)' % (time/1e6, threadid, coreid, ret_val, emulated)

sim.util.register(LogSyscalls())
