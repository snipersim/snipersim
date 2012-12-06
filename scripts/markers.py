# Monitor SimMarker() calls made by the application.

import sim
class Marker:
  def setup(self, args):
    args = (args or '').split(':')
    self.write_terminal = 'verbose' in args
    self.write_markers = 'markers' in args
    self.write_stats = 'stats' in args
  def hook_magic_marker(self, thread, core, a, b, s):
    if self.write_terminal:
      print '[SCRIPT] Magic marker from thread %d: a = %d,' % (thread, a),
      if s:
        print 'str = %s' % s
      else:
        print 'b = %d' % b

    if self.write_markers:
      sim.stats.marker(thread, core, a, b, s)

    # Pass in 'stats' as option to write out statistics at each magic marker
    # $ run-sniper -s markers:stats
    # This will allow for e.g. partial CPI stacks of specific code regions:
    # $ tools/cpistack.py --partial=marker-1-4:marker-2-4

    if self.write_stats:
      sim.stats.write('marker-%d-%d' % (a, b))

sim.util.register(Marker())
