"""
Regression fixture for sim.stats.register() callbacks.

This is the path through py_stats.cc:statsCallback().  Unlike every callback in
py_hooks.cc it used to call into CPython without holding the GIL, which made the
simulator segfault on the first simulator-initiated statistics snapshot.  Keep
this test in the regression suite: the failure only appears when the embedded
interpreter has threads, so it cannot be caught by inspection.
"""
import os
import sys

# run-sniper inserts only the hook script's own directory on sys.path, so a
# fixture living outside $SNIPER_ROOT/scripts has to find the sim package itself.
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'scripts'))

import sim


class StatsCallbackProbe:
  def setup(self, args):
    self.calls = 0
    for core in range(sim.config.ncores):
      sim.stats.register('core', core, 'statscb_probe', self.cb)

  def cb(self, objectName, index, metricName):
    self.calls += 1
    # Allocate and free on the Python heap on every call, so a missing GIL
    # guard corrupts state rather than merely racing benignly.
    scratch = [(objectName, index, metricName, i) for i in range(64)]
    return len(scratch) + self.calls

  def hook_sim_end(self):
    print('[STATSCB] statsCallback invocations: %d' % self.calls)


sim.util.register(StatsCallbackProbe())
