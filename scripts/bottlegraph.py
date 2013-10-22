import sim, os, collections

# On-line collection of bottle graphs [Du Bois, OOSPLA 2013]
# See tools/bottlegraph.py for a version that works on off-line data

class BottleGraph:
  def setup(self, args):
    self.total_runtime = 0
    self.runtime = collections.defaultdict(long)
    self.contrib = collections.defaultdict(long)
    self.running = collections.defaultdict(bool)
    self.time_last = 0
    self.in_roi = False

  def update(self, time):
    if not self.in_roi:
      return
    nthreads = sum([ 1 if running else 0 for running in self.running.values() ])
    if time > self.time_last:
      time_interval = (time - self.time_last) / 1e6 # Count everything in nanoseconds
      self.total_runtime += time_interval
      if nthreads:
        for thread, running in self.running.items():
          if running:
            self.runtime[thread] += time_interval
            self.contrib[thread] += time_interval / float(nthreads)
      self.time_last = time

  def hook_roi_begin(self):
    self.time_last = sim.stats.time()
    self.in_roi = True

  def hook_roi_end(self):
    self.update(sim.stats.time())
    self.in_roi = False

  def hook_thread_start(self, threadid, time):
    self.update(time)
    self.running[threadid] = True

  def hook_thread_exit(self, threadid, time):
    self.update(time)
    self.running[threadid] = False

  def hook_thread_stall(self, threadid, reason, time):
    if reason != 'unscheduled':
      self.update(time)
      self.running[threadid] = False

  def hook_thread_resume(self, threadid, threadby, time):
    self.update(time)
    self.running[threadid] = True

  def hook_sim_end(self):
    xs = dict([ (thread, self.runtime[thread] / self.contrib[thread]) for thread in self.runtime ])
    ys = dict([ (thread, self.contrib[thread] / 1e9) for thread in self.runtime ])
    runtime = self.total_runtime / 1e9
    # Threads in descending order of parallelism
    threads = sorted(self.runtime.keys(), key = lambda thread: xs[thread], reverse = True)

    #for thread in threads:
    #  print '[BOTTLEGRAPH]', thread, '%.5f' % ys[thread], '%.5f' % xs[thread], sim.thread.get_thread_name(thread)

    max_x = int(max(xs.values()) + 1.2)
    max_y = runtime * 1.1
    fd = open('%s/bottlegraph.input' % sim.config.output_dir, 'w')
    print >> fd, '''\
set terminal png font "FreeSans,10" size 450,400
set output "bottlegraph.png"
set grid
set xlabel "Parallelism"
set ylabel "Runtime (seconds)"
set key outside bottom horizontal
set style fill solid 1.0 noborder
set xrange [-%d:%d]
set yrange [0:%f]
set xtics (%s) nomirror
''' % (max_x, max_x, max_y, ','.join([ '"%d" %d' % (abs(x), x) for x in range(-max_x, max_x+1) ]))

    y = 0
    colors = ('#00FFFF', '#A52A2A', '#A9A9A9', '#FF1493', '#8FBC8F', '#FF6347', '#006400')
    color = lambda i: colors[i % len(colors)]

    for i, thread in enumerate(threads):
      print >> fd, 'set object rect from %f,%f to %f,%f fc rgb "%s"' % (-xs[thread], y, xs[thread], y+ys[thread], color(i))
      y += ys[thread]

    print >> fd, 'plot %s' % ', '.join([
      '-1 with boxes title "%s" lc rgb "%s"' % (sim.thread.get_thread_name(thread), color(i))
      for i, thread in reversed(list(enumerate(threads)))
      if ys[thread] > .01 * runtime
    ])
    fd.close()

    os.system('cd "%s" && gnuplot bottlegraph.input' % sim.config.output_dir)


sim.util.register(BottleGraph())
