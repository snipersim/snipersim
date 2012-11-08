"""
stattrace.py

Write a trace of deltas for an arbitrary statistic.
First argument is the name of the statistic (<component-name>[.<subcomponent>].<stat-name>)
Second argument is either a filename, or none to write to standard output
Third argument is the interval size in nanoseconds (default is 10000)
"""

import sys, os, sim

class StatTrace:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    stat = args[0]
    filename = args.get(1, None)
    interval_ns = long(args.get(2, 10000))

    if '.' not in stat:
      print 'Stat name needs to be of the format <component>.<statname>, now %s' % stat
      return
    self.stat_name = stat
    stat_component, stat_name = stat.rsplit('.', 1)
    try:
      sim.stats.get(stat_component, 0, stat_name)
    except ValueError:
      print 'Stat %s[*].%s not found' % (stat_component, stat_name)
      return

    if filename:
      self.fd = file(os.path.join(sim.config.output_dir, filename), 'w')
      self.isTerminal = False
    else:
      self.fd = sys.stdout
      self.isTerminal = True

    self.sd = sim.util.StatsDelta()
    self.stats = {
      'time': [ self.sd.getter('performance_model', core, 'elapsed_time') for core in range(sim.config.ncores) ],
      'ffwd_time': [ self.sd.getter('fastforward_performance_model', core, 'fastforwarded_time') for core in range(sim.config.ncores) ],
      'stat': [ self.sd.getter(stat_component, core, stat_name) for core in range(sim.config.ncores) ],
    }
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, statsdelta = self.sd, roi_only = True)

  def periodic(self, time, time_delta):
    if self.isTerminal:
      self.fd.write('[STAT:%s] ' % self.stat_name)
    self.fd.write('%u' % (time / 1e6)) # Time in ns
    for core in range(sim.config.ncores):
      timediff = (self.stats['time'][core].delta - self.stats['ffwd_time'][core].delta) / 1e6 # Time in ns
      statdiff = self.stats['stat'][core].delta
      value = statdiff / (timediff or 1) # Avoid division by zero
      self.fd.write(' %.3f' % value)
    self.fd.write('\n')


sim.util.register(StatTrace())
