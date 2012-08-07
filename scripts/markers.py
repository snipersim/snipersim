# Monitor SimMarker() calls made by the application.

import sim
class Marker:
  def hook_magic_marker(self, thread, core, a, b):
    print '[SCRIPT] Magic marker from thread %d: a = %d, b = %d' % (thread, a, b)

    # Uncomment to write out statistics at each magic marker
    # This will allow for e.g. partial CPI stacks of specific code regions:
    # $ tools/cpistack.py --partial=marker-1-4:marker-2-4

    #sim.stats.write('marker-%d-%d' % (a, b))

sim.util.register(Marker())
