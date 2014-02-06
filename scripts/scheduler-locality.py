import sim


def getScoreMetricTime(thread_id):
  return long(sim.stats.get('thread', thread_id, 'nonidle_elapsed_time'))

def getScoreMetricInstructions(thread_id):
  return long(sim.stats.get('thread', thread_id, 'instruction_count'))


class Thread:

  def __init__(self, thread_id, getScoreMetric):
    self.thread_id = thread_id
    self.getScoreMetric = lambda: getScoreMetric(thread_id)
    self.core = None
    self.runnable = False
    self.unscheduled = False
    self.score = 0       # Accumulated score
    self.metric_last = 0 # State at start of last interval
    sim.thread.set_thread_affinity(self.thread_id, ())

  def updateScore(self):
    metric_now = self.getScoreMetric()
    self.score += metric_now - self.metric_last
    self.metric_last = metric_now

  def setScore(self, score):
    self.score = score
    self.metric_last = self.getScoreMetric()

  def setCore(self, core_id, time = -1):
    self.core = core_id
    if core_id is None:
      self.updateScore()
      self.last_scheduled_out = time
      sim.thread.set_thread_affinity(self.thread_id, ())
    else:
      self.last_scheduled_in = time
      sim.thread.set_thread_affinity(self.thread_id, [ c == core_id for c in range(sim.config.ncores) ])

  def __repr__(self):
    return 'Thread(%d, %s, score = %d)' % (self.thread_id, 'core = %d' % self.core if self.core is not None else 'no core', self.score)


class SchedulerLocality:

  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval_ns = long(args.get(0, None) or 10000000)
    scheduler_type = args.get(1, 'equal_time')
    core_mask = args.get(2, '')
    if scheduler_type == 'equal_time':
      self.getScoreMetric = getScoreMetricTime
    elif scheduler_type == 'equal_instructions':
      self.getScoreMetric = getScoreMetricInstructions
    else:
      raise ValueError('Invalid scheduler type %s' % scheduler_type)
    if core_mask:
      core_mask = map(int, core_mask.split(',')) + [0]*sim.config.ncores
      self.cores = [ core for core in range(sim.config.ncores) if core_mask[core] ]
    else:
      self.cores = range(sim.config.ncores)
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic)
    self.threads = {}
    self.last_core = 0

  def hook_thread_start(self, thread_id, time):
    self.threads[thread_id] = Thread(thread_id, self.getScoreMetric)
    self.threads[thread_id].runnable = True
    # Initial assignment: one thread per core until cores are exhausted
    if self.last_core < len(self.cores):
      self.threads[thread_id].setCore(self.cores[self.last_core], sim.stats.time())
      self.last_core += 1
    else:
      self.threads[thread_id].setCore(None, sim.stats.time())

  def hook_thread_exit(self, thread_id, time):
    self.hook_thread_stall(thread_id, 'exit', time)

  def hook_thread_stall(self, thread_id, reason, time):
    if reason == 'unscheduled':
      # Ignore calls due to the thread being scheduled out
      self.threads[thread_id].unscheduled = True
    else:
      core = self.threads[thread_id].core
      self.threads[thread_id].setCore(None, time)
      self.threads[thread_id].runnable = False
      # Schedule a new thread (runnable, but not running) on this free core
      threads = [ thread for thread in self.threads.values() if thread.runnable and thread.core is None ]
      if threads:
        # Order by score
        threads.sort(key = lambda thread: thread.score)
        threads[0].setCore(core, time)

  def hook_thread_resume(self, thread_id, woken_by, time):
    if self.threads[thread_id].unscheduled:
      # Ignore calls due to the thread being scheduled back in
      self.threads[thread_id].unscheduled = False
    else:
      self.threads[thread_id].setScore(min([ thread.score for thread in self.threads.values() ]))
      self.threads[thread_id].runnable = True
      # If there is a free core, move us there now
      used_cores = set([ thread.core for thread in self.threads.values() if thread.core is not None ])
      free_cores = set(self.cores) - used_cores
      if len(free_cores):
        self.threads[thread_id].setCore(list(free_cores)[0], time)

  def periodic(self, time, time_delta):
    # Update thread scores
    [ thread.updateScore() for thread in self.threads.values() if thread.core is not None ]

    # Get a list of all runnable threads
    threads = [ thread for thread in self.threads.values() if thread.runnable ]
    # Order by score
    threads.sort(key = lambda thread: thread.score)
    # Select threads to run now, one per core
    threads = threads[:len(self.cores)]
    #print ', '.join(map(repr, threads))

    # Filter out threads that are already running, and keep them on their current core
    keep_threads = [ thread for thread in threads if thread.core is not None ]
    used_cores = set([ thread.core for thread in keep_threads ])

    # Move new threads to free cores
    free_cores = set(self.cores) - used_cores
    threads = [ thread for thread in threads if thread.core is None ]
    assert(len(free_cores) >= len(threads))

    for thread, core in zip(threads, sorted(free_cores)):
      current_thread = [ t for t in self.threads.values() if t.core == core ]
      if current_thread:
        current_thread[0].setCore(None)
      thread.setCore(core, time)
      assert thread.runnable


sim.util.register(SchedulerLocality())
