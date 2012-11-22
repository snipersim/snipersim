"""
periodicins-stats.py

Periodically write out all statistics (instruction-based)
1st argument is the interval size in instructions (default is 1e9)
"""

import sim

class PeriodicInsStats:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval = long(args.get(0, '') or 1000000000)
    sim.util.EveryIns(long(interval), self.periodicins, roi_only = True)

  def periodicins(self, ins, ins_delta):
    sim.stats.write('periodicins-%d' % ins)

sim.util.register(PeriodicInsStats())
