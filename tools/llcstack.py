#!/usr/bin/env python2

import sys, os, getopt, sniper_lib, sniper_stats

def usage():
  print 'Usage:', sys.argv[0], '[-h (help)] [--partial <section-start>:<section-end> (default: roi-begin:roi-end)]  [-d <resultsdir (default: .)>]'


jobid = 0
resultsdir = '.'
partial = None

try:
  opts, args = getopt.getopt(sys.argv[1:], "hj:d:", [ 'partial=' ])
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

if args:
  usage()
  sys.exit(-1)


results = sniper_lib.get_results(jobid, resultsdir, partial = partial)
config = results['config']
stats = results['results']

ncores = int(config['general/total_cores'])
llc_number = int(config['perf_model/cache/levels'])
llc_name = 'L%d' % llc_number
llc_components = [ name.split('.', 1)[1] for name in sorted(stats.keys()) if '.uncore-time-' in name ]

totaltime = stats['%s.uncore-totaltime' % llc_name]
requests = stats['%s.uncore-requests' % llc_name]
sums = [ 0 for core in range(ncores) ]

def format_num(v):
  return '%8d' % v
def format_ns(v):
  return '%8.2f' % (v * 1e-6)

print '                        Average | ' + '  '.join(map(lambda core: '%8s' % ('Core %d' % core), range(ncores)))
print
print 'Requests:              ' + format_num(sum(requests)) + ' | ' + '  '.join(map(format_num, requests))
print 'Total time:            ' + format_ns(sum(totaltime) / (sum(requests) or 1)) + ' | ' + '  '.join(map(lambda t, n: format_ns(t / (n or 1)), totaltime, requests))
print
for component in llc_components:
  statname = '%s.%s' % (llc_name, component)
  if sum(stats[statname]):
    print '%-22s' % ('%s:' % component[12:]),
    print format_ns(sum(stats[statname]) / (sum(requests) or 1)) + ' |',
    for core in range(ncores):
      print format_ns(stats[statname][core] / (requests[core] or 1)) + ' ',
      sums[core] += stats[statname][core]
    print
if sum(totaltime) > sum(sums):
  print 'unaccounted:           ' + format_ns((sum(totaltime) - sum(sums)) / (sum(requests) or 1)) + ' |',
  print '  '.join(map(lambda t, s, n: format_ns((t - s) / (n or 1)), totaltime, sums, requests))
