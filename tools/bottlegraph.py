#!/usr/bin/env python2

# Generation of bottle graphs [Du Bois, OOSPLA 2013]

import sys, os, collections, getopt, sniper_lib, sniper_stats


def translateThreadNameJikes(name):
  name = name.replace('org.mmtk.plan.generational.immix.GenImmixCollector', 'GcThread')
  name = name.replace('org.mmtk.plan.ControllerCollectorContext', 'ControllerThread')
  return name


def bottlegraph(jobid = None, resultsdir = None, outputfile = './bottlegraph', partial = None, no_text = False, thread_names_translate = translateThreadNameJikes):
  stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
  results = stats.get_results(partial = partial)['results']

  thread_names = dict([ (threadid, 'Thread-%d' % threadid) for threadid in range(len(results['thread.bottle_runtime_time'])) ])
  for threadid, name in stats.get_thread_names().items():
    thread_names[threadid] = thread_names_translate(name)

  runtime = dict(enumerate(results['thread.bottle_runtime_time']))
  contrib = dict(enumerate(results['thread.bottle_contrib_time']))
  total_runtime = results['barrier.global_time'][0] / 1e15

  xs = dict([ (thread, runtime[thread] / float(contrib[thread])) for thread in runtime if runtime[thread] > 0 ])
  ys = dict([ (thread, contrib[thread] / 1e15) for thread in runtime if runtime[thread] > 0 ])
  # Threads in descending order of parallelism
  threads = sorted(xs.keys(), key = lambda thread: xs[thread], reverse = True)

  if not no_text:
    print 'Runtime (s)   Parallelism   Thread name'
    for thread in threads:
      print '%11.5f' % ys[thread], '%13.2f' % xs[thread], ' '*3, thread_names[thread]

  max_x = int(max(xs.values()) + 1.2)
  max_y = total_runtime * 1.1
  fd = open('%s.input' % outputfile, 'w')
  print >> fd, '''\
set terminal png font "FreeSans,10" size 450,400
set output "%s.png"
set grid
set xlabel "Parallelism"
set ylabel "Runtime (seconds)"
set key outside bottom horizontal
set style fill solid 1.0 noborder
set xrange [-%d:%d]
set yrange [0:%f]
set xtics (%s) nomirror
''' % (os.path.basename(outputfile), max_x, max_x, max_y, ','.join([ '"%d" %d' % (abs(x), x) for x in range(-max_x, max_x+1) ]))

  y = 0
  colors = ('#00FFFF', '#A52A2A', '#A9A9A9', '#FF1493', '#8FBC8F', '#FF6347', '#006400')
  color = lambda i: colors[i % len(colors)]

  for i, thread in enumerate(threads):
    print >> fd, 'set object rect from %f,%f to %f,%f fc rgb "%s"' % (-xs[thread], y, xs[thread], y+ys[thread], color(i))
    y += ys[thread]

  print >> fd, 'plot %s' % ', '.join([
    '-1 with boxes title "%s" lc rgb "%s"' % (thread_names[thread], color(i))
    for i, thread in reversed(list(enumerate(threads)))
    if ys[thread] > .01 * total_runtime
  ])
  fd.close()

  os.system('cd "%s" && gnuplot %s.input' % (os.path.dirname(outputfile), os.path.basename(outputfile)))



if __name__ == '__main__':

  def usage():
    print 'Usage:', sys.argv[0], '[-h (help)] [--partial <section-start>:<section-end> (default: roi-begin:roi-end)]  [-d <resultsdir (default: .)>]  [-o <output (default: ./bottlegraph.png)>]'


  jobid = 0
  resultsdir = '.'
  outputfile = './bottlegraph'
  partial = None
  no_text = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hj:d:o:", [ 'partial=', 'no-text' ])
  except getopt.GetoptError, e:
    print e
    usage()
    sys.exit()
  for o, a in opts:
    if o == '-h':
      usage()
      sys.exit()
    if o == '-d':
      resultsdir = a
    if o == '-j':
      jobid = long(a)
    if o == '-o':
      outputfile = a
    if o == '--partial':
      if ':' not in a:
        sys.stderr.write('--partial=<from>:<to>\n')
        usage()
      partial = a.split(':')
    if o == '--no-text':
      no_text = True

  if args:
    usage()
    sys.exit(-1)

  bottlegraph(jobid, resultsdir, outputfile, partial = partial, no_text = no_text)
