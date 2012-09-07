import sim

class HpiTest:
  def setup(self, args):
    sim.util.EveryIns(1000000, self.periodic_ins)

  def periodic_ins(self, icount, icount_delta):
    print '[HOOK_PERIODIC_INS]', icount, icount_delta

sim.util.register(HpiTest())
