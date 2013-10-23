"""
ipctrace.py

Write a trace of instantaneous IPC values for all cores.
First argument is either a filename, or none to write to standard output.
Second argument is the interval size in nanoseconds (default is 10000)
"""

import sys, os, sim

class IpcTrace:
  def setup(self, args):
    self.freq = sim.dvfs.get_frequency(0) # This script does not support DVFS
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
      'threadinstrs': [],
      'threadtime': [],
    }
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, statsdelta = self.sd, roi_only = True)

  def hook_thread_start(self, threadid, creator):
    for thread in range(len(self.stats['threadinstrs']), threadid+1):
      self.stats['threadinstrs'].append(self.sd.getter('thread', thread, 'instruction_count'))
      self.stats['threadtime'].append(self.sd.getter('thread', thread, 'elapsed_time'))

  def periodic(self, time, time_delta):
    if self.isTerminal:
      self.fd.write('[THREADIPC] ')
    self.fd.write('%u' % (time / 1e6)) # Time in ns
    for thread in range(sim.thread.get_nthreads()):
      # Print per-thread stats
      try:
        cycles = self.stats['threadtime'][thread].delta * self.freq / 1e9 # convert fs to cycles
        instrs = self.stats['threadinstrs'][thread].delta
        ipc = instrs / (cycles or 1)
        self.fd.write(' %.3f' % ipc)
      except TypeError:
        pass # Skip newly created threads
    self.fd.write('\n')

sim.util.register(IpcTrace())
