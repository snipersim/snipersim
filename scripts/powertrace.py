"""
Write out all statistics every 1M cycles and run a partial McPAT
"""

import sys, os, sim

class PowerTrace:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval_ns = long(args.get(0, '') or 1000000)
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, roi_only = True)
    self.t_last = 0

  def periodic(self, time, time_delta):
    time = long(time/1e6) # fs to ns
    if time <= 100:
      # ignore first callback which is at 100ns
      return
    sim.stats.write(str(time)) # write to sim.stats with prefix 'time'
    self.do_power(self.t_last, time)
    self.t_last = time

  def hook_roi_end(self):
    self.t_roi_end = long(long(sim.stats.get('performance_model', 0, 'elapsed_time'))/1e6)

  def hook_sim_end(self):
    self.do_power(self.t_last, None)

  def do_power(self, t0, t1):
    _t0 = t0 or 'roi-begin'
    _t1 = t1 or 'roi-end'
    if not t1: t1 = self.t_roi_end
    os.system('unset PYTHONHOME; %s -d %s -o %s --partial=%s:%s --no-graph' % (
      os.path.join(os.getenv('SNIPER_ROOT'), 'tools/mcpat.py'),
      sim.config.output_dir,
      os.path.join(sim.config.output_dir, 'power-%s-%s-%s' % (t0, t1, t1 - t0)),
      _t0, _t1
    ))

sim.util.register(PowerTrace())
