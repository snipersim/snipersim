# End ROI after x instructions (aggregated over all cores), with configurable x (default 1B) and optional warmup
# Combine with --no-cache-warming to use fast-forward rather than cache warmup
# Usage: -s stop-by-icount:30000000                            # Start in detailed, and end after 30M instructions
#        -s stop-by-icount:30000000 --roi                      # Start in warmup, switch to detailed at application ROI marker, and end after 30M instructions
#        -s stop-by-icount:30000000:100000000 --roi-script     # Start in cache-warmup, switch to detailed after 100M instructions, and run for 30M in detailed
#        -s stop-by-icount:30000000:roi+100000000 --roi-script # Start in cache-warmup, wait for application ROI, switch to detailed after 100M instructions, and run for 30M in detailed
import sim

class StopByIcount:
  def _min_callback(self):
    return min(self.ninstrs_start or (), self.ninstrs, self.min_ins_global)
  def setup(self, args):
    self.magic = sim.config.get_bool('general/magic')
    self.min_ins_global = long(sim.config.get('core/hook_periodic_ins/ins_global'))
    self.wait_for_app_roi = False
    self.verbose = False
    args = dict(enumerate((args or '').split(':')))
    self.ninstrs = long(args.get(0, 1e9))
    start = args.get(1, None)
    roirelstart = False
    # Make the start input value canonical
    if start == '':
      start = None
    roiscript = sim.config.get_bool('general/roi_script')
    if start == None and not roiscript:
      if self.magic:
        self.roi_rel = True
        self.ninstrs_start = 0
        self.inroi = False
        print '[STOPBYICOUNT] Waiting for application ROI'
      else:
        self.roi_rel = True
        self.ninstrs_start = 0
        self.inroi = True
        print '[STOPBYICOUNT] Starting in ROI (detail)'
    else:
      if self.magic:
        print '[STOPBYICOUNT] ERROR: Application ROIs and warmup cannot be combined when using --roi'
        print '[STOPBYICOUNT] Use syntax: -s stop-by-icount:NDETAIL:roi+NWARMUP --roi-script'
        sim.control.abort()
        self.done = True
        return
      if not roiscript:
        print '[STOPBYICOUNT] ERROR: --roi-script is not set, but is required when using a start instruction count. Aborting'
        sim.control.abort()
        self.done = True
        return
      if start == None:
        # If start == None, then an explicit start has not been set, but --roi-script has also been enabled.
        # Therefore, set to start on the next instruction callback
        print '[STOPBYICOUNT] WARNING: No explicit start instruction count was set, but --roi-script is set'
        print '               WARNING: Starting detailed simulation on the next callback, %d instructions' % self.min_ins_global
        print '               WARNING: To start from the beginning, do not use --roi-script with a single stop argument'
        start = self.min_ins_global
      if start.startswith('roi+'):
        self.ninstrs_start = long(start[4:])
        self.roi_rel = True
        self.wait_for_app_roi = True
        print '[STOPBYICOUNT] Starting %s instructions after ROI begin' % self.ninstrs_start
      else:
        self.ninstrs_start = long(start)
        self.roi_rel = False
        print '[STOPBYICOUNT] Starting after %s instructions' % self.ninstrs_start
      self.inroi = False
    print '[STOPBYICOUNT] Then stopping after simulating %s instructions in detail' % ((self.roi_rel and 'at least ' or '') + str(self.ninstrs))
    self.done = False
    sim.util.EveryIns(self._min_callback(), self.periodic, roi_only = (start == None))
  def hook_application_roi_begin(self):
    if self.wait_for_app_roi:
      print '[STOPBYICOUNT] Application at ROI begin, fast-forwarding for', self.ninstrs_start, 'more instructions'
      self.wait_for_app_roi = False
      self.ninstrs_start = sim.stats.icount() + self.ninstrs_start
  def hook_roi_begin(self):
    if self.magic:
      self.ninstrs_start = sim.stats.icount()
      self.inroi = True
      print '[STOPBYICOUNT] Application ROI started, now simulating', self.ninstrs, 'in detail'
  def periodic(self, icount, icount_delta):
    if self.done:
      return
    if self.verbose:
      print '[STOPBYICOUNT] Periodic at', icount, ' delta =', icount_delta
    if self.inroi and icount > (self.ninstrs + self.ninstrs_start):
      print '[STOPBYICOUNT] Ending ROI after %s instructions (%s requested)' % (icount - self.ninstrs_start, self.ninstrs)
      sim.control.set_roi(False)
      self.inroi = False
      self.done = True
      sim.control.abort()
    elif not self.inroi and not self.wait_for_app_roi and icount > self.ninstrs_start:
      print '[STOPBYICOUNT] Starting ROI after %s instructions' % icount
      sim.control.set_roi(True)
      self.inroi = True
sim.util.register(StopByIcount())
