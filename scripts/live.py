"""
live.py -- the run's live feed.

One JSON record per line in <output_dir>/sim.live, written as the
simulation goes, in simulated time.  Tail it while the run is going; the
dashboard reads it, and so does anything that wants to know what the
machine and its threads were doing when.

  -s live[:interval_ns[:path]]   sample interval in simulated ns (default 100000);
                                 path: where to write (default <output_dir>/sim.live).
                                 A named pipe streams the feed without storing it:
                                 tools/sniper-live --run does this for you.

Records, by their "ev" field:
  header  once: cores, frequencies, the CPI groups being sampled, interval
  sample  every interval: per core -- instructions retired, time spent in
          detailed and in fast-forward mode, and the CPI stack as fs per
          group (the groups are cpistack.py's, so the live stack and the
          post-hoc one agree); and the thread table (state, core, name)
  thread  start / exit / stall (with reason) / resume / migrate
  mode    instrumentation mode change (detailed, fast-forward, cache-only)
  os      the software layer: an event posted to a core's MimicOS queue
          (page_fault, new_thread, thread_exit, quantum_expired, shutdown)
          or a kernel worker switching to an application thread (switch_to,
          arg = the app thread id)
  roi     roi-begin / roi-end
  marker  a SimMarker from the application
  end     simulation end

Times are simulated femtoseconds ("t"); "null" when a hook has no time.
"""
import sys, os, json, sim

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import cpistack_items

MAXTIME = 2**63          # hooks pass SubsecondTime::MaxTime() when the time is not known
TIMERS = ('rob_timer', 'interval_timer', 'fastforward_timer', 'performance_model')
MODES = {1: 'detailed', 2: 'cache-only', 3: 'fast-forward'}          # inst_mode.h
OS_EVENTS = {1: 'page_fault', 2: 'syscall', 3: 'new_thread', 4: 'thread_exit', 5: 'quantum_expired', 6: 'shutdown', 100: 'switch_to'}

def flatten(items, group = None):
  """(group, leaf) for every leaf of cpistack_items' tree; group is the top-level name."""
  for name, threshold, contrib in items:
    g = group or name
    if isinstance(contrib, str):
      yield g, contrib
    elif isinstance(contrib, tuple):
      for leaf in contrib: yield g, leaf
    else:
      for x in flatten(contrib, g): yield x

class Live:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.interval_ns = int(args.get(0, '') or 100000)
    self.ncores = sim.config.ncores
    path = args.get(1, '') or os.path.join(sim.config.output_dir, 'sim.live')
    self.fd = self.open_feed(path)
    self.threads = {}                     # thread id -> {'state', 'core', 'reason', 'by'}
    self.sd = sim.util.StatsDelta()
    self.instrs = [ self.sd.getter('performance_model', c, 'instruction_count') for c in range(self.ncores) ]
    self.time   = [ self.sd.getter('performance_model', c, 'elapsed_time') for c in range(self.ncores) ]
    self.ffwd   = [ self.sd.getter('fastforward_performance_model', c, 'fastforwarded_time') for c in range(self.ncores) ]
    # The CPI stack: every leaf that exists on this run's timer, grouped as cpistack.py groups them.
    self.cpi = {}                         # group -> [ [getters for core 0], [core 1], ... ]
    for group, leaf in flatten(cpistack_items.build_itemlist()):
      for timer in TIMERS:
        try:
          getters = [ self.sd.getter(timer, c, 'cpi' + leaf) for c in range(self.ncores) ]
        except ValueError:
          continue
        for c in range(self.ncores):
          self.cpi.setdefault(group, [ [] for _ in range(self.ncores) ])[c].append(getters[c])
        break
    self.emit(dict(ev = 'header', t = 0, ncores = self.ncores, interval_ns = self.interval_ns,
                   freq_mhz = [ sim.dvfs.get_frequency(c) for c in range(self.ncores) ],
                   cpi_groups = sorted(self.cpi.keys())))
    sim.util.Every(self.interval_ns * sim.util.Time.NS, self.sample, statsdelta = self.sd, roi_only = False)
    # The hooks: sim.util.register() wires every hook_<name> method below to HOOK_<NAME>.

  def open_feed(self, path):
    """A file, or a named pipe.  A pipe is opened without blocking: if no
    reader shows up within a few seconds (the dashboard was closed, or never
    started) the feed goes to sim.live instead and the run is not held up."""
    if os.path.exists(path) and not os.path.isfile(path):
      import time, fcntl
      for _ in range(50):
        try:
          fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:            # ENXIO: no reader yet
          time.sleep(0.1)
          continue
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_NONBLOCK)
        print('[live] streaming to %s' % path)
        return os.fdopen(fd, 'w', buffering = 1)
      path = os.path.join(sim.config.output_dir, 'sim.live')
      print('[live] no reader on the pipe; the feed goes to %s' % path)
    return open(path, 'w', buffering = 1)

  def emit(self, rec):
    if not self.fd:
      return
    try:
      self.fd.write(json.dumps(rec, separators = (',', ':')) + '\n')
    except (BrokenPipeError, OSError):
      self.fd = None        # the reader went away (a dashboard quit); the run goes on without the feed

  def when(self, t):
    return None if t is None or t >= MAXTIME else t

  def thread_table(self):
    out = []
    for tid in sorted(self.threads):
      th = dict(self.threads[tid]); th['id'] = tid
      try:
        th['name'] = sim.thread.get_thread_name(tid)
        th['app'] = sim.thread.get_thread_appid(tid)
      except Exception: pass
      out.append(th)
    return out

  def sample(self, time, time_delta):
    cores = []
    for c in range(self.ncores):
      stack = { g: sum(m.delta for m in getters[c]) for g, getters in self.cpi.items() }
      cores.append(dict(instrs = self.instrs[c].delta, time = self.time[c].delta, ffwd = self.ffwd[c].delta, cpi = stack))
    self.emit(dict(ev = 'sample', t = time, dt = time_delta, cores = cores, threads = self.thread_table()))

  # -- thread lifecycle: the table is kept from the hooks, and each change is also an event --
  def hook_thread_start(self, tid, creator):
    self.threads[tid] = dict(state = 'running', core = None)
    self.emit(dict(ev = 'thread', t = None, id = tid, what = 'start', by = creator))
  def hook_thread_exit(self, tid, time):
    self.threads.pop(tid, None)
    self.emit(dict(ev = 'thread', t = self.when(time), id = tid, what = 'exit'))
  def hook_thread_stall(self, tid, reason, time):
    th = self.threads.setdefault(tid, dict(core = None)); th['state'] = 'stalled'; th['reason'] = reason
    self.emit(dict(ev = 'thread', t = self.when(time), id = tid, what = 'stall', reason = reason))
  def hook_thread_resume(self, tid, by, time):
    th = self.threads.setdefault(tid, dict(core = None)); th['state'] = 'running'; th.pop('reason', None)
    self.emit(dict(ev = 'thread', t = self.when(time), id = tid, what = 'resume', by = by))
  def hook_thread_migrate(self, tid, core, time):
    th = self.threads.setdefault(tid, dict(state = 'running')); th['core'] = core
    self.emit(dict(ev = 'thread', t = self.when(time), id = tid, what = 'migrate', core = core))

  def hook_instrument_mode(self, mode):
    self.emit(dict(ev = 'mode', t = sim.stats.time(), mode = MODES.get(mode, mode)))
  def hook_roi_begin(self):
    self.emit(dict(ev = 'roi', t = sim.stats.time(), what = 'begin'))
  def hook_roi_end(self):
    self.emit(dict(ev = 'roi', t = sim.stats.time(), what = 'end'))
  def hook_magic_marker(self, tid, core, a, b, s):
    self.emit(dict(ev = 'marker', t = sim.stats.time(), id = tid, core = core, a = a, b = b, s = s))
  def hook_mimicos_event(self, core, kind, arg, time):
    self.emit(dict(ev = 'os', t = self.when(time), core = core, kind = OS_EVENTS.get(kind, kind), arg = arg))
  def hook_sim_end(self):
    # Not closed: threads still exit after this, and the feed keeps them.
    self.emit(dict(ev = 'end', t = sim.stats.time(), icount = sim.stats.icount()))

sim.util.register(Live())
