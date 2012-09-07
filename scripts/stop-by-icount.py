# End ROI after x instructions (aggregated over all cores), with configurable x (default 1B)
import sim

class StopByIcount:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.ninstrs = long(args.get(0, 1e9))
    self.icount_init = 0
    self.stats = [ sim.stats.getter('performance_model', core, 'instruction_count') for core in range(sim.config.ncores) ]
    sim.util.Every(sim.util.Time.US, self.periodic, roi_only = True)
  def roi_begin(self):
    self.icount_init = sum([ long(stat()) for stat in self.stats ])
  def periodic(self, time, time_delta):
    icount = sum([ long(stat()) for stat in self.stats ])
    if icount - self.icount_init > self.ninstrs:
      print '[SCRIPT] Ending ROI after %d instructions' % (icount - self.icount_init)
      sim.control.set_roi(False)
sim.util.register(StopByIcount())
