"""
periodic-stats.py

Periodically write out all statistics
Argument is the interval size in nanoseconds (default is 1e9 = 1 second of simulated time)
"""

import sim

class PeriodicStats:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval_ns = long(args.get(0, 1e9))
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, roi_only = True)

  def periodic(self, time, time_delta):
    sim.stats.write('periodic-%d' % time)

sim.util.register(PeriodicStats())
