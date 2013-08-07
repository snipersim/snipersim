import sim

class SyncTrace:
  def hook_thread_start(self, threadid, time):
    print '[SYNC]', threadid, 'from app', sim.thread.get_thread_appid(threadid), 'start at', time / 1000000 # Time in ns

  def hook_thread_exit(self, threadid, time):
    print '[SYNC]', threadid, 'exit at', time / 1000000

  def hook_thread_stall(self, threadid, reason, time):
    print '[SYNC]', threadid, 'sleep for', reason, 'at', time / 1000000

  def hook_thread_resume(self, threadid, threadby, time):
    print '[SYNC]', threadid, 'woken by', threadby, 'at', time / 1000000

  def hook_thread_migrate(self, threadid, coreid, time):
    print '[SYNC]', threadid, 'scheduled to', coreid, 'at', time / 1000000

sim.util.register(SyncTrace())
