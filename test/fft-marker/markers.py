# Monitor SimMarker() calls made by the application.

import sim
class Marker:
  def hook_magic_marker(self, core, thread, a, b):
    print '[SCRIPT] Magic marker from thread %d: a = %d, b = %d' % (thread, a, b)
sim.util.register(Marker())
