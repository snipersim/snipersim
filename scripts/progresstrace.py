import sim, time

class ProgressTrace:
  def setup(self, args):
    self.last_ins = 0
    self.last_time = 0
    sim.util.EveryIns(1000, self.periodic, roi_only = False)

  def periodic(self, ins, ins_delta):
    time_now = time.time()
    time_delta = time_now - self.last_time
    if time_delta >= 1:
      ins_delta = ins - self.last_ins
      ips = ins_delta / time_delta
      print '[PROGRESS] %.0fM instructions, %.0f KIPS' % (ins / 1e6, ips / 1e3)
      self.last_ins = ins
      self.last_time = time_now

sim.util.register(ProgressTrace())
