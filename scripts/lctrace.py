"""
lctrace.py

Write a trace of LightCache miss rates (in misses per 1000 instructions) for all cores.
First argument is either a filename, or none to write to standard output.
Second argument is an interval in nanoseconds (default = 10000)
"""

import sys, os, sim

class LCTrace:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    filename = args.get(0, None)
    interval_ns = long(args.get(1, 10000))
    if filename:
      self.fd = file(os.path.join(sim.config.output_dir, filename), 'w')
      self.isTerminal = False
    else:
      self.fd = sys.stdout
      self.isTerminal = True
    self.sd = sim.util.StatsDelta()
    self.stats = {
      'time': [ self.sd.getter('performance_model', core, 'elapsed_time') for core in range(sim.config.ncores) ],
      'instrs': [ self.sd.getter('core', core, 'instructions') for core in range(sim.config.ncores) ],
      'misses': [ self.sd.getter('light_cache', core, 'misses') for core in range(sim.config.ncores) ],
    }
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, statsdelta = self.sd, roi_only = True)

  def periodic(self, time, time_delta):
    if self.isTerminal:
      self.fd.write('[LC] ')
    self.fd.write('%u' % (time / 1e6)) # Time in ns
    for core in range(sim.config.ncores):
      instrs = self.stats['instrs'][core].delta
      misses = self.stats['misses'][core].delta
      rate = 1000 * misses / (instrs or 1)
      self.fd.write(' %.1f' % rate)
    self.fd.write('\n')


sim.util.register(LCTrace())
