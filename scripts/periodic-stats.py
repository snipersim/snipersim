"""
periodic-stats.py

Periodically write out all statistics
1st argument is the interval size in nanoseconds (default is 1e9 = 1 second of simulated time)
2nd argument, if "ins" then interval is number of instructions rather than nanoseconds
"""

import sim

class PeriodicStats:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval = long(args.get(0, 1e9))
    by_ins = args.get(1, '')
    if by_ins == 'ins':
      sim.util.EveryIns(interval, self.periodic, roi_only = True)
    else:
      sim.util.Every(interval * sim.util.Time.NS, self.periodic, roi_only = True)

  def periodic(self, time, time_delta):
    sim.stats.write('periodic-%d' % time)

sim.util.register(PeriodicStats())
