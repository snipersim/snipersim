import sys, sim

"""
Conversion factors for subsecondtime (femtoseconds) to other units
"""

class Time:
  FS = 1.
  PS = 1e3
  NS = 1e6
  US = 1e9
  MS = 1e12
  S  = 1e15



"""
Register an object as being a hooks listener.
  Will register hooks callback functions to the object's member functions with matching (lowercase) names
  I.e. obj.hook_roi_begin will be called at HOOK_ROI_BEGIN, etc.
  Additionally, obj.setup(arg) will be called with <arg> being the script's arguments
"""

def register(obj):

  for name, hook in sim.hooks.hooks.items():
    func = getattr(obj, name.lower(), None)
    if func and callable(func):
      sim.hooks.register(hook, func)

  if hasattr(obj, 'setup') and callable(obj.setup):
    obj.setup(sys.argv[1])



"""
Register a callback for a given SimUser command.
  E.g.: Python script calls sim.util.register_command(0x123, myfunc)
        Application calls SimUser(0x123, <arg>) from core <core>
        Python callback is made for myfunc(<core>, <arg>)
        Return value of SimUser() is that of myfunc()
"""

def register_command(mycmd, func):
  def callback(thread, core, cmd, arg):
    if cmd == mycmd:
      return func(core, arg)
  sim.hooks.register(sim.hooks.HOOK_MAGIC_USER, callback)



"""
Delta manager for statistics.
  StatsDeltaMetric keeps the current, last, and delta value for a given statistic
  StatsDelta keeps a list of StatsDeltaMetric metrics and updates them all at once

Example usage:

  import sim, simutil

  class PrintIpc:
    def __init__(self):
      self.sd = simutil.StatsDelta()
      self.instrs = self.sd.getter("performance_model", 0, "instruction_count")
      self.time = self.sd.getter("performance_model", 0, "elapsed_time")

    def hook_periodic(self, time):
      if self.sd.update(): # returns False the first time (no delta available)
        cycles = self.time.delta * sim.dvfs.get_frequency(0) / 1e6 # convert fs to cycles
        print self.instrs.delta / (cycles or 1)

  simutil.register(PrintIpc())
"""

class StatsDelta:

  class StatsDeltaMetric:
    """Internal object to store current, last and delta stats value.

    Do not instantiate directly, use StatsDelta.getter() instead."""
    def __init__(self, objectName, index, metricName):
      self.getter = sim.stats.getter(objectName, index, metricName)
      self.last = None
      self.delta = None

    def update(self):
      now = float(self.getter())
      if self.last is not None:
        self.delta = now - self.last
      self.last = now

  def __init__(self):
    self.isFirst = True
    self.members = []

  def getter(self, objectName, index, metricName):
    getter = self.StatsDeltaMetric(objectName, index, metricName)
    self.members.append(getter)
    return getter

  def update(self):
    for member in self.members:
      member.update()
    if self.isFirst:
      self.isFirst = False
      return False
    else:
      return True


class Every:
  def __init__(self, interval, callback, statsdelta = None, roi_only = True):
    min_interval = long(sim.config.get('clock_skew_minimization/barrier/quantum')) * 1e6
    if interval < min_interval:
      print >> sys.stderr, 'sim.util.Every(): interval(%dns) < periodic callback(%dns), consider reducing clock_skew_minimization/barrier/quantum' % (interval/1e6, min_interval/1e6)
    self.interval = interval
    self.callback = callback
    self.statsdelta = statsdelta
    self.roi_only = roi_only
    self.time_next = 0
    self.time_last = 0
    self.in_roi = False
    register(self)

  def hook_roi_begin(self):
    self.in_roi = True
    self.hook_periodic(sim.stats.time())

  def hook_roi_end(self):
    self.hook_periodic(sim.stats.time())
    self.in_roi = False

  def hook_periodic(self, time):
    if (not self.roi_only or self.in_roi) and time >= self.time_next:
      time_delta = time - self.time_last
      self.time_next = time + self.interval
      self.time_last = time

      if self.statsdelta:
        doCall = self.statsdelta.update()
      else:
        doCall = True

      if doCall:
        self.callback(time, time_delta)


class EveryIns:
  def __init__(self, interval, callback, roi_only = True):
    min_interval = long(sim.config.get('core/hook_periodic_ins/ins_global'))
    if interval < min_interval:
      print >> sys.stderr, 'sim.util.EveryIns(): interval(%d) < periodic callback(>=%d), consider reducing core/hook_periodic_ins/ins_global' % (interval, min_interval)
    self.interval = interval
    self.callback = callback
    self.roi_only = roi_only
    self.icount_next = interval
    self.icount_last = 0
    self.in_roi = False
    register(self)

  def hook_roi_begin(self):
    self.in_roi = True

  def hook_roi_end(self):
    self.in_roi = False

  def hook_periodic_ins(self, icount):
    if (not self.roi_only or self.in_roi) and icount >= self.icount_next:
      icount_delta = icount - self.icount_last
      self.icount_next += self.interval
      self.icount_last = icount

      self.callback(icount, icount_delta)
