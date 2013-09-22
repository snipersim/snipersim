import sim

class DemoScheduler:
  def setup(self, args):
    self.core_mapping = dict((core, '-') for core in range(sim.config.ncores))
    self.icount_last = [ 0 for core in range(sim.config.ncores) ]
    self.ipc = [ 0 for core in range(sim.config.ncores) ]
    self.last_reschedule = 0
    sim.util.Every(10 * sim.util.Time.US, self.periodic, roi_only = True)

  def hook_thread_migrate(self, threadid, coreid, time):
    self.core_mapping[coreid] = threadid

  def periodic(self, time, time_delta):
    if time_delta:
      for core in range(sim.config.ncores):
        icount = sim.stats.get('performance_model', core, 'instruction_count')
        icount_diff = icount - self.icount_last[core]
        cycles = time_delta * sim.dvfs.get_frequency(core) / 1e9
        self.icount_last[core] = icount
        self.ipc[core] = icount_diff / cycles
      self.print_info()
    if time - self.last_reschedule >= 50e9:
      self.reschedule()
      self.last_reschedule = time

  def print_info(self):
    print '[t=%3dus]' % (sim.stats.time() / 1e9),
    print 'mapping:', ' '.join(str(self.core_mapping[core]) for core in range(sim.config.ncores)),
    print '   ipc:', ' '.join('%3.1f' % self.ipc[core] for core in range(sim.config.ncores)),
    print '   idle:',
    for core in range(sim.config.ncores):
      print '%2.0f%%' % (100 * sim.stats.get('performance_model', core, 'idle_elapsed_time') / float(sim.stats.time())),
    print '   threads:',
    for thread in range(sim.thread.get_nthreads()):
      print '%5dkins' % (sim.stats.get('thread', thread, 'instruction_count') / 1e3),
    print

  def hook_roi_end(self):
    print
    print '[DEMO] Total runtime = %d us' % (sim.stats.time() / 1e9)
    print

  def reschedule(self):
    if False:
      threads = [ (thread, sim.stats.get('thread', thread, 'instruction_count')) for thread in range(sim.thread.get_nthreads()) ]
      threads.sort(key = lambda (thread, icount): icount, reverse = False)
      print '[DEMO] Reschedule:',
      for i, (thread, _) in enumerate(threads):
        affinity = [ core == i for core in range(sim.config.ncores) ]
        sim.thread.set_thread_affinity(thread, affinity)
        print '%d=>%d' % (thread, i),
      print

sim.util.register(DemoScheduler())
