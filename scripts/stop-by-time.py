# End ROI after x nanoseconds
# Usage: -s stop-by-time:1000000                # End after 1 ms of simulated time

import sim

class StopByTime:

  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.time = long(args.get(0, 1e6)) * 1e6 # ns to fs
    self.done = False
    sim.util.Every(sim.util.Time.US, self.periodic, roi_only = True)

  def periodic(self, time, time_delta):
    if time < self.time:
      sim.control.set_progress(time / float(self.time or 1))
    if self.done:
      return
    elif time >= self.time:
      print '[STOPBYTIME] Ending ROI after %.0f nanoseconds' % (time / 1e6)
      sim.control.set_roi(False)
      self.done = True
      sim.control.abort()

sim.util.register(StopByTime())
