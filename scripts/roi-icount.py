# Scripted ROI start/end based on instruction counts.
#
# This script will switch to warmup after X instructions,
# run in cache-only mode for Y instructions,
# run Z instructions in detailed mode,
# and then fast-forward to the end.
# X can be roi+X for ROI-relative start
#
# run-sniper --roi-script --no-cache-warming -s roi-icount:X:Y:Z
#
# Start the simulation with "--roi-script --no-cache-warming"
# to start in fast-forward mode and ignore SimRoi{Start,End}

import sim

class RoiIcount:

  def _min_callback(self):
    offsets = [ self.init_length, self.warmup_length, self.detailed_length ]
    offsets = filter(lambda v: v not in (0, -1), offsets)
    return min(offsets)

  def setup(self, args):
    if args:
      args = dict(enumerate(args.split(':')))
    else:
      args = {}
    start = args.get(0, '')
    if start.startswith('roi+'):
      start = start[4:]
      self.roirel = True
    else:
      self.roirel = False
    self.init_length = long(start or 0)
    self.warmup_length = long(args.get(1, '') or 0)
    self.detailed_length = long(args.get(2, '') or 0)

    if self.detailed_length < 1:
      print >> sys.stderr, '[ROI-ICOUNT] Detailed instrucion count cannot be 0'
      sim.control.abort()
    if sim.config.get_bool('general/magic') or not sim.config.get_bool('general/roi_script'):
      print >> sys.stderr, '[ROI-ICOUNT] Need --roi-script to be set, --roi must not be used'
      sim.control.abort()

    min_interval = long(sim.config.get('core/hook_periodic_ins/ins_global'))
    if self._min_callback() < min_interval:
      print '[ROI-ICOUNT] Periodic instruction callback too short (%d), consider reducing core/hook_periodic_ins/ins_global' % min_interval

    if self.roirel:
      print '[ROI-ICOUNT] Waiting for application ROI'
    if self.init_length:
      print '[ROI-ICOUNT] Fast-forward for %d instructions' % self.init_length
    if self.warmup_length:
      print '[ROI-ICOUNT] Warmup for %d instructions' % self.warmup_length
    else:
      print '[ROI-ICOUNT] No warmup'
    print '[ROI-ICOUNT] Detailed region of %d instructions' % self.detailed_length

    if self.roirel:
      self.state = 'preroi'
    else:
      self.state = 'init'
      self.offset = 0
    sim.util.EveryIns(long(sim.config.get('core/hook_periodic_ins/ins_global')), self.periodic_ins, roi_only = False)

  def hook_sim_start(self):
    # Jump straight into warmup/detailed if their offset is zero
    self.periodic_ins(0, 0)

  def hook_application_roi_begin(self):
    if self.state == 'preroi':
      self.state = 'init'
      self.offset = sim.stats.icount()
      print '[ROI-ICOUNT] Icount = %d, Application ROI' % self.offset

  def periodic_ins(self, icount, icount_delta):
    if self.state == 'done':
      return
    if self.state == 'preroi':
      return

    if self.state == 'detailed' and icount >= self.offset + self.init_length + self.warmup_length + self.detailed_length:
      print '[ROI-ICOUNT] Icount = %d: ending ROI' % icount
      sim.control.set_roi(False)
      self.state = 'done'

    if self.state in ('init', 'warmup') and icount >= self.offset + self.init_length + self.warmup_length:
      print '[ROI-ICOUNT] Icount = %d: beginning ROI' % icount
      sim.control.set_roi(True)
      self.state = 'detailed'

    if self.state == 'init' and icount >= self.offset + self.init_length:
      print '[ROI-ICOUNT] Icount = %d: going to WARMUP' % icount
      sim.control.set_instrumentation_mode(sim.control.WARMUP)
      self.state = 'warmup'

sim.util.register(RoiIcount())
