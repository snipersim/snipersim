# This script will switch to warmup for iterations 0 & 1, detailed for 2 & 3, and then fast-forward to the end
# Start the simulation with --roi-script --no-cache-warming, so we start in fast-forward mode and ignore SimRoi{Start,End}

import sim
class Iter2:
  def hook_magic_marker(self, core, thread, a, b):
    if a == 1 and b == 0:
      print '[SCRIPT] Start of iteration 0: going to WARMUP'
      sim.control.set_instrumentation_mode(sim.control.WARMUP)
    elif a == 1 and b == 2:
      print '[SCRIPT] Start of iteration 2: beginning ROI'
      sim.control.set_roi(True)
    elif a == 2 and b == 3:
      print '[SCRIPT] End of iteration 3: ending ROI'
      sim.control.set_roi(False)
sim.util.register(Iter2())
