#!/usr/bin/env python

import os, sys, re, collections, gnuplot, getopt
import cpistack_data, cpistack_items, cpistack_results

try:
  collections.defaultdict()
except AttributeError, e:
  print sys.argv[0], "Error: This script requires Python version 2.5 or greater"
  sys.exit()


# New-style function returning CPI stack results
def cpistack_compute(jobid = 0, resultsdir = '.', data = None, partial = None,
                     cores_list = None, core_mincomp = 0., aggregate = False,
                     items = None, groups = None, use_simple = False, use_simple_mem = True,
                     no_collapse = False):

  # Get the data
  cpidata = cpistack_data.CpiData(jobid = jobid, resultsdir = resultsdir, data = data, partial = partial)
  cpidata.filter(cores_list = cores_list, core_mincomp = core_mincomp)
  if aggregate:
    cpidata.aggregate()

  # Build the structure descriptor
  cpiitems = cpistack_items.CpiItems(items = items, groups = groups, use_simple = use_simple, use_simple_mem = use_simple_mem)

  # Group data according to descriptor
  results = cpistack_results.CpiResults(cpidata, cpiitems, no_collapse = no_collapse)

  return results


def output_cpistack_text(results):
  print '                        CPI       Time'
  cpi = results.get_data('cpi')
  time = results.get_data('time')

  for core in results.cores:
    if len(results.cores) > 1:
      print 'Core', core
    total = { 'cpi': 0, 'time': 0 }
    for label in results.labels:
      print '  %-15s    %6.2f    %6.2f%%' % (label, cpi[core][label], 100 * time[core][label])
      total['cpi'] += cpi[core][label]
      total['time'] += time[core][label]
    print
    print '  %-15s    %6.2f    %6.2f%%' % ('total', total['cpi'], 100 * total['time'])


def output_cpistack_gnuplot(results, metric = 'time', outputfile = 'cpi-stack', outputdir = '.', title = '', size = (640, 480)):
  # Use Gnuplot to make stacked bargraphs of these cpi-stacks
  plot_data = results.get_data(metric)
  colors = results.get_colors()
  plot_labels_with_color = [ (label, 'rgb "#%02x%02x%02x"' % colors[label]) for label in results.labels ]
  gnuplot.make_stacked_bargraph(os.path.join(outputdir, outputfile), plot_labels_with_color, plot_data, size = size, title = title,
    ylabel = metric == 'cpi' and 'Cycles per instruction' or (metric == 'abstime' and 'Time (seconds)' or 'Fraction of time'))


if __name__ == '__main__':
  def usage():
    print 'Usage:', sys.argv[0], '[-h|--help (help)] [-j <jobid> | -d <resultsdir (default: .)>] [-o <output-filename (cpi-stack)>] [--title=""] [--without-roi] [--simplified] [--no-collapse] [--no-simple-mem] [--time|--cpi|--abstime (default: time)] [--aggregate]'

  jobid = 0
  resultsdir = '.'
  partial = None
  outputfile = 'cpi-stack'
  title = ''
  metric = 'time'
  use_simple = False
  use_simple_mem = True
  no_collapse = False
  aggregate = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hj:d:o:", [ "help", "title=", "no-roi", "simplified", "no-collapse", "no-simple-mem", "cpi", "time", "abstime", "aggregate", "partial=" ])
  except getopt.GetoptError, e:
    print e
    usage()
    sys.exit()
  for o, a in opts:
    if o == '-h' or o == '--help':
      usage()
      sys.exit()
    if o == '-d':
      resultsdir = a
    if o == '-j':
      jobid = long(a)
    if o == '-o':
      outputfile = a
    if o == '--title':
      title = a
    if o == '--no-roi':
      partial = ('start', 'stop')
    if o == '--simplified':
      use_simple = True
    if o == '--no-collapse':
      no_collapse = True
    if o == '--no-simple-mem':
      use_simple_mem = False
    if o == '--time':
      pass
    if o == '--cpi':
      metric = 'cpi'
    if o == '--abstime':
      metric = 'abstime'
    if o == '--aggregate':
      aggregate = True
    if o == '--partial':
      if ':' not in a:
        sys.stderr.write('--partial=<from>:<to>\n')
        usage()
      partial = a.split(':')

  if args:
    usage()
    sys.exit(-1)

  results = cpistack_compute(jobid = jobid, resultsdir = resultsdir, partial = partial, aggregate = aggregate,
                             groups = use_simple and cpistack_items.build_grouplist(legacy = True) or None,
                             use_simple = use_simple, use_simple_mem = use_simple_mem, no_collapse = no_collapse)

  output_cpistack_text(results)
  output_cpistack_gnuplot(results = results, metric = metric, outputfile = outputfile, title = title)
