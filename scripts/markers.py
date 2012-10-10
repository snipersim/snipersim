# Monitor SimMarker() calls made by the application.

import sim
class Marker:
  def setup(self, args):
    args = (args or '').split(',')
    if 'stats' in args:
      self.write_stats = True
    else:
      self.write_stats = True
  def hook_magic_marker(self, thread, core, a, b):
    print '[SCRIPT] Magic marker from thread %d: a = %d, b = %d' % (thread, a, b)

    # Pass in 'stats' as option to write out statistics at each magic marker
    # $ run-sniper -s markers:stats
    # This will allow for e.g. partial CPI stacks of specific code regions:
    # $ tools/cpistack.py --partial=marker-1-4:marker-2-4

    if self.write_stats:
      sim.stats.write('marker-%d-%d' % (a, b))

sim.util.register(Marker())
