"""
output-as-markers.py

Record application output to stdout/stderr as markers.
Arguments are value for A, value for B, and prefix for string in marker data.
"""

import sim, syscall_strings

class OutputAsMarkers:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.value_a = int(args.get(0, 0) or 0)
    self.value_b = int(args.get(1, 0))
    self.prefix = args.get(2, '')
  def hook_syscall_enter(self, threadid, coreid, time, syscall_number, args):
    syscall_name = syscall_strings.syscall_strings.get(syscall_number, 'unknown')
    if syscall_name == 'write':
      fd, ptr, size = args[:3]
      if fd == 1 or fd == 2:
        prefix = {1: 'stdout', 2: 'stderr'}[fd]
        data = sim.mem.read(coreid, ptr, size)
        sim.stats.marker(threadid, coreid, self.value_a, self.value_b, '%s%s: %s' % (self.prefix, prefix, data))

sim.util.register(OutputAsMarkers())
