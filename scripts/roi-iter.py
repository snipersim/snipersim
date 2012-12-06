# Scripted ROI start/end based on SimMarker iteration markers.
# Assumes start and end of each iteration is marked using SimMarker(1, i) and SimMarker(2, i), respectively.
#
# This script will switch to warmup at the beginning of iteration A (default: first SimMarker(1, *)),
# run in detailed from the start of iteration B until the end of iteration C,
# and then fast-forward to the end.
#
# run-sniper --roi-script --no-cache-warming -s roi-iter:A:B:C
#
# Start the simulation with "--roi-script --no-cache-warming" to start in fast-forward mode and ignore SimRoi{Start,End}

import sim

class RoiIter:
  def setup(self, args):
    if args:
      args = dict(enumerate(args.split(':')))
    else:
      args = {}
    self.start_warmup = int(args.get(0, '') or -1)
    self.start_detailed = int(args.get(1, '') or -1)
    self.end_detailed = int(args.get(2, '') or -1)
    self.state = 'init'
  def hook_magic_marker(self, core, thread, a, b, s):
    if self.state == 'done':
      # Only enable once, even if the benchmark has duplicate SimMarker()s
      return
    if a == 2 and b == self.end_detailed:
      print '[SCRIPT] End of iteration %d: ending ROI' % b
      sim.control.set_roi(False)
      self.state = 'done'
    elif a == 1 and (b == self.start_detailed or (self.start_detailed == -1 and self.state != 'detailed')):
      print '[SCRIPT] Start of iteration %d: beginning ROI' % b
      sim.control.set_roi(True)
      self.state = 'detailed'
    elif a == 1 and (b == self.start_warmup or (self.start_warmup == -1 and self.state == 'init')):
      print '[SCRIPT] Start of iteration %d: going to WARMUP' % b
      sim.control.set_instrumentation_mode(sim.control.WARMUP)
      self.state = 'warmup'

sim.util.register(RoiIter())
