"""
bbvtrace.py

Write a trace of BBV deltas for all cores.
Argument is either a filename, or none to write to standard output.
"""

import sys, os, sim

class Bbv:
  def __init__(self, init = None):
    if init:
      self.ninstrs = init[0]
      self.bbv = list(init[1])
    else:
      self.ninstrs = 0
      self.bbv = [ 0 for i in range(sim.bbv.BBV_SIZE) ]
  def delta(self, bbv):
    delta = Bbv()
    delta.ninstrs = bbv.ninstrs - self.ninstrs;
    for i in range(sim.bbv.BBV_SIZE):
      delta.bbv[i] = bbv.bbv[i] - self.bbv[i]
    return delta
  def diff(self, bbv):
    diff = 0
    for i in range(sim.bbv.BBV_SIZE):
      diff += abs(bbv.bbv[i]/(bbv.ninstrs or 1) - self.bbv[i]/(self.ninstrs or 1))
    return diff

class BbvTrace:
  def setup(self, args):
    sim.bbv.enable()
    args = dict(enumerate((args or '').split(':')))
    filename = args.get(0, None)
    interval_ns = long(args.get(1, 100000))
    if filename:
      self.fd = file(os.path.join(sim.config.output_dir, filename), 'w')
      self.isTerminal = False
    else:
      self.fd = sys.stdout
      self.isTerminal = True
    self.bbvprev = [ Bbv() for i in range(sim.config.ncores) ]
    self.deltaprev = [ Bbv() for i in range(sim.config.ncores) ]
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, roi_only = True)

  def periodic(self, time, time_delta):
    if self.isTerminal:
      self.fd.write('[BBV] ')
    self.fd.write('%u' % (time / 1e6)) # Time in ns
    for core in range(sim.config.ncores):
      bbv = Bbv(sim.bbv.get(core))
      delta = self.bbvprev[core].delta(bbv)
      diff = self.deltaprev[core].diff(delta)
      self.bbvprev[core] = bbv
      self.deltaprev[core] = delta
      self.fd.write(' %d' % diff)
    self.fd.write('\n')


sim.util.register(BbvTrace())
