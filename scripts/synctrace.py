import sim

class SyncTrace:
  def hook_thread_stall(self, coreid, time):
    print coreid, 'sleep at', time

  def hook_thread_resume(self, coreid, coreby, time):
    print coreid, 'woken by', coreby, 'at', time

sim.util.register(SyncTrace())
