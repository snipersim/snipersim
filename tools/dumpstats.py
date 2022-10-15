#!/usr/bin/env python2

import sys, os, getopt, sniper_lib, sniper_stats

def usage():
  print 'Usage:', sys.argv[0], '[-h (help)] [-l|--list | -t|--topology | -m|--markers | -e|--events | -c|--config ] [--partial <section-start>:<section-end> (default: roi-begin:roi-end)] [--through-time|tt <statname>]  [-d <resultsdir (default: .)>]'


jobid = 0
resultsdir = '.'
partial = None
through_time = None
do_list = False
do_topo = False
do_markers = False
do_events = False
do_stats = True
do_config = False

try:
  opts, args = getopt.getopt(sys.argv[1:], "hj:d:lmtec", [ 'list', 'markers', 'topology', 'events', 'config', 'partial=', 'tt=', 'through-time=' ])
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
  if o == '--partial':
    if ':' not in a:
      sys.stderr.write('--partial=<from>:<to>\n')
      usage()
    partial = a.split(':')
  if o in ('--tt', '--through-time'):
    through_time = a.split(',')
  if o in ('-l', '--list'):
    do_list = True
    do_stats = False
  if o in ('-t', '--topology'):
    do_topo = True
    do_stats = False
  if o in ('-m', '--markers'):
    do_markers = True
    do_stats = False
  if o in ('-e', '--events'):
    do_events = True
    do_stats = False
  if o in ('-c', '--config'):
    do_config = True
    do_stats = False

if args:
  usage()
  sys.exit(-1)



def format_event(timestamp, core, thread, message):
  return '%9ld ns: core(%2d) thread(%2d)  %s' % (timestamp / 1e6, core, thread, message)

def format_marker(value0, value1, description):
  if description:
    return 'a = %3d,  str = "%s"' % (value0, description)
  else:
    return 'a = %3d,  b = %3d' % (value0, value1)


if do_list:
  import sniper_stats
  stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
  print ', '.join(stats.get_snapshots())

if do_topo:
  import sniper_stats
  stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
  for t in stats.get_topology():
    print ', '.join(map(str,t))

if do_markers:
  import sniper_stats
  stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
  try:
    markers = stats.get_markers()
  except Exception, e:
    print >> sys.stderr, e
    print >> sys.stderr, "--markers could not be fetched"
    sys.exit(1)

  for timestamp, core, thread, value0, value1, description in markers:
    print format_event(timestamp, core, thread, format_marker(value0, value1, description))

if do_events:
  import sniper_stats
  stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
  try:
    events = stats.get_events()
  except Exception, e:
    print >> sys.stderr, e
    print >> sys.stderr, "--events could not be fetched"
    sys.exit(1)

  for event, timestamp, core, thread, value0, value1, description in events:
    if event == sniper_stats.EVENT_MARKER:
      print format_event(timestamp, core, thread, 'Marker: %s' % format_marker(value0, value1, description))
    elif event == sniper_stats.EVENT_THREAD_NAME:
      print format_event(timestamp, core, thread, 'Thread name: %s' % description)
    elif event == sniper_stats.EVENT_APP_START:
      print format_event(timestamp, core, thread, 'Application %d start' % value0)
    elif event == sniper_stats.EVENT_APP_EXIT:
      print format_event(timestamp, core, thread, 'Application %d exit' % value0)
    elif event == sniper_stats.EVENT_THREAD_CREATE:
      print format_event(timestamp, core, thread, 'Thread created: application %d by thread %d' % (value0, value1))
    elif event == sniper_stats.EVENT_THREAD_EXIT:
      print format_event(timestamp, core, thread, 'Thread exit')
    else:
      print format_event(timestamp, core, thread, 'Unknown event %d (%d, %d, %s)' % (event, value0, value1, description))

if do_config:
  config = sniper_lib.get_config(resultsdir = resultsdir, jobid = jobid)
  for k, v in sorted(config.items()):
    print '%s=%s' % (k, v)


if do_stats:

  def print_result(key, value):
    if type(value) is dict:
      for _key, _value in sorted(value.items()):
        print_result(key+'.'+_key, _value)
    else:
      print key, '=',
      if type(value) is list:
        print ', '.join(map(str, value))
      else:
        print value

  if through_time:
    import sniper_stats
    stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
    names = stats.read_metricnames()
    metrics = [ metric[1:] if metric[0] in '-' else metric for metric in through_time ]
    nameids = dict([ ('%s.%s' % (objectname, metricname), nameid) for nameid, (objectname, metricname) in names.items() if '%s.%s' % (objectname, metricname) in metrics ])
    prefixes = stats.get_snapshots()
    prefixes_len = max(map(len, prefixes))
    data = dict([ (prefix, stats.read_snapshot(prefix, metrics)) for prefix in prefixes ])

    def do_op(op, state, v):
      if op == '-':
        for i, _v in enumerate(v):
          v[i], state[i] = v[i] - state.get(i, 0), v[i]
        return v
      else:
        return v

    with sniper_lib.OutputToLess():
      for metric, _metric in zip(metrics, through_time):
        op = _metric[0]
        print '==', metric, '=='
        state = {}
        for prefix in prefixes:
          v = data[prefix].get(nameids[metric], {})
          v = [ v.get(i, 0) for i in range(max(v.keys() or [0])+1) ]
          v = do_op(op, state, v)
          print_result('%-*s' % (prefixes_len, prefix), v)

  else:
    results = sniper_lib.get_results(jobid, resultsdir, partial = partial)

    with sniper_lib.OutputToLess():
      for key, value in sorted(results['results'].items(), key = lambda (key, value): key.lower()):
        print_result(key, value)
