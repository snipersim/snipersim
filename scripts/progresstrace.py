import sim, time

class ProgressTrace:
  def setup(self, args):
    self.last_ins = 0
    self.last_time = 0
    sim.util.EveryIns(1000000, self.periodic, roi_only = False)
    self.sd = sim.util.StatsDelta()
    self.stats = {
      'time': self.sd.getter('barrier', 0, 'global_time'),
      'instrs': [ self.sd.getter('core', core, 'instructions') for core in range(sim.config.ncores) ],
    }

  def periodic(self, ins, ins_delta):
    time_now = time.time()
    time_delta = time_now - self.last_time
    if time_delta >= 1:
      ins_delta = ins - self.last_ins
      ips = ins_delta / time_delta
      self.sd.update()
      if self.stats['time'].delta:
        cycles = self.stats['time'].delta * sim.dvfs.get_frequency(0) / 1e9 # convert fs to cycles
        instrs = sum([ self.stats['instrs'][core].delta for core in range(sim.config.ncores) ])
        ipc = instrs / (cycles or 1)
      else:
        ipc = None
      print ('[PROGRESS] %.0fM instructions, %.0f KIPS' % (ins / 1e6, ips / 1e3)) + (', %.2f IPC' % ipc if ipc else '')
      self.last_ins = ins
      self.last_time = time_now

sim.util.register(ProgressTrace())
