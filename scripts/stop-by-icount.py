# End ROI after x instructions (aggregated over all cores), with configurable x (default 1B)
# Usage: -s stop-by-icount:30000000                        # Start in ROI, detailed, and end after 30M instructions
# Usage: -s stop-by-icount:30000000:100000000 --roi-script # Start in cache-warmup, non-ROI, switch to ROI after 100M instructions, and run for 30M in detailed ROI
import sim

class StopByIcount:
  def _min_callback(self):
    return min(self.ninstrs_start or (), self.ninstrs, long(1000000))
  def setup(self, args):
    self.verbose = False
    args = dict(enumerate((args or '').split(':')))
    self.ninstrs = long(args.get(0, 1e9))
    start = args.get(1, None)
    # Make the start input value canonical
    if start == '':
      start = None
    if start == None:
      self.roi_rel = True
      self.ninstrs_start = None
      self.inroi = True
      print '[STOPBYICOUNT] Starting at ROI'
    else:
      self.roi_rel = False
      self.ninstrs_start = long(start)
      self.inroi = False
      roiscript = sim.config.get_bool('general/roi_script')
      if not roiscript:
        print '[STOPBYICOUNT] ERROR: --roi-script is not set, but is required when using a start instruction count. Aborting.'
        sim.control.abort()
        self.done = True
        return
      print '[STOPBYICOUNT] Starting after %s instructions' % self.ninstrs_start
    print '[STOPBYICOUNT] Then stopping after %s instructions' % ((self.roi_rel and 'at least ' or '') + str(self.ninstrs))
    self.done = False
    sim.util.EveryIns(self._min_callback(), self.periodic, roi_only = (start == None))
  def periodic(self, icount, icount_delta):
    if self.done:
      return
    if self.verbose:
      print '[STOPBYICOUNT] Periodic at', icount, ' delta =', icount_delta
    # For ROI-relative starts, setup an approximate starting instruction count
    if self.ninstrs_start == None:
      min_delta = self._min_callback()
      self.ninstrs_start = (icount / min_delta) * min_delta
    if self.inroi and icount > (self.ninstrs + self.ninstrs_start):
      print '[STOPBYICOUNT] Ending ROI after %s instructions' % ((self.roi_rel and 'at least ' or '') + str(icount))
      sim.control.set_roi(False)
      self.inroi = False
      self.done = True
      sim.control.abort()
    elif not self.inroi and icount > self.ninstrs_start:
      print '[STOPBYICOUNT] Starting ROI after %s instructions' % ((self.roi_rel and 'at least ' or '') + str(icount))
      sim.control.set_roi(True)
      self.inroi = True
sim.util.register(StopByIcount())
