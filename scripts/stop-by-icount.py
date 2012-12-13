# End ROI after x instructions (aggregated over all cores), with configurable x (default 1B)
# Usage: -s stop-by-icount:30000000                        # Start in ROI, detailed, and end after 30M instructions
# Usage: -s stop-by-icount:30000000:100000000 --roi-script # Start in cache-warmup, non-ROI, switch to ROI after 100M instructions, and run for 30M in detailed ROI
import sim

class StopByIcount:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.ninstrs = long(args.get(0, 1e9))
    start = args.get(1, None)
    if start == None:
      self.ninstrs_start = 0
      self.inroi = True
    else:
      self.ninstrs_start = long(start)
      self.inroi = False
    sim.util.EveryIns(long(1000000), self.periodic, roi_only = (start == None))
  def periodic(self, icount, icount_delta):
    print 'Periodic at', icount
    if self.inroi and icount > (self.ninstrs + self.ninstrs_start):
      print '[SCRIPT] Ending ROI after %d instructions' % icount
      sim.control.set_roi(False)
      self.inroi = False
    elif not self.inroi and icount > self.ninstrs_start:
      print '[SCRIPT] Starting ROI after %d instructions' % icount
      sim.control.set_roi(True)
      self.inroi = True
sim.util.register(StopByIcount())
