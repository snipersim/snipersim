#!/usr/bin/env python

import sys, os, getopt, sniper_lib, sniper_stats

def usage():
  print 'Usage:', sys.argv[0], '[-h (help)] [-l|--list | -t|--topology | -m|--markers] [--partial <section-start>:<section-end> (default: roi-begin:roi-end)] [--through-time|tt <statname>]  [-d <resultsdir (default: .)>]'


jobid = 0
resultsdir = '.'
partial = None
through_time = None
do_list = False
do_topo = False
do_markers = False
do_stats = True

try:
  opts, args = getopt.getopt(sys.argv[1:], "hj:d:lmt", [ 'list', 'markers', 'topology', 'partial=', 'tt=', 'through-time=' ])
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

if args:
  usage()
  sys.exit(-1)


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
    if description:
      marker = 'a = %3d,  str = "%s"' % (value0, description)
    else:
      marker = 'a = %3d,  b = %3d' % (value0, value1)
    print '%9ld ns: core(%2d) thread(%2d)  %s' % (timestamp / 1e6, core, thread, marker)


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
    metrics = through_time
    nameids = dict([ ('%s.%s' % (objectname, metricname), nameid) for nameid, (objectname, metricname) in names.items() if '%s.%s' % (objectname, metricname) in metrics ])
    prefixes = stats.get_snapshots()
    prefixes_len = max(map(len, prefixes))
    data = dict([ (prefix, stats.read_snapshot(prefix, metrics)) for prefix in prefixes ])

    with sniper_lib.OutputToLess():
      for metric in metrics:
        print '==', metric, '=='
        for prefix in prefixes:
          v = data[prefix][nameids[metric]]
          v = [ v.get(i, 0) for i in range(max(v.keys())+1) ]
          print_result('%-*s' % (prefixes_len, prefix), v)

  else:
    results = sniper_lib.get_results(jobid, resultsdir, partial = partial)

    with sniper_lib.OutputToLess():
      for key, value in sorted(results['results'].items(), key = lambda (key, value): key.lower()):
        print_result(key, value)
