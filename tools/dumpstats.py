#!/usr/bin/env python

import sys, os, getopt, sniper_lib, sniper_stats

def usage():
  print 'Usage:', sys.argv[0], '[-h (help)] [--list] [--partial <section-start>:<section-end> (default: roi-begin:roi-end)]  [<resultsdir (default: .)>]'


jobid = 0
resultsdir = '.'
partial = None
do_list = False

try:
  opts, args = getopt.getopt(sys.argv[1:], "hj:d:", [ 'list', 'partial=' ])
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
  if o == '--list':
    do_list = True

if args:
  usage()
  sys.exit(-1)


if do_list:
  if jobid:
    print >> sys.stderr, "--list not supported with jobid"
    sys.exit(1)
  else:
    import sniper_stats
    stats = sniper_stats.SniperStats(resultsdir)
    print ', '.join(stats.get_snapshots())
    sys.exit(0)


results = sniper_lib.get_results(jobid, resultsdir, partial = partial)


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

for key, value in sorted(results['results'].items()):
  print_result(key, value)
