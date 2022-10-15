#!/usr/bin/env python2
# -*- coding: utf8 -*-

import os, sys, re, collections, gnuplot, getopt
import cpistack_data, cpistack_items, cpistack_results

try:
  collections.defaultdict()
except AttributeError, e:
  print sys.argv[0], "Error: This script requires Python version 2.5 or greater"
  sys.exit()


# New-style function returning CPI stack results
def cpistack_compute(jobid = 0, resultsdir = None, config = None, stats = None, data = None, partial = None,
                     cores_list = None, core_mincomp = 0., aggregate = False,
                     items = None, groups = None, use_simple = False, use_simple_mem = True,
                     no_collapse = False):

  # Get the data
  cpidata = cpistack_data.CpiData(jobid = jobid, resultsdir = resultsdir, config = config, stats = stats, data = data, partial = partial)
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


def output_cpistack_table(results, metric = 'cpi'):
  data = results.get_data(metric)
  total = [ 0 for core in results.cores ]
  multiplier = 1.

  if metric == 'time':
    format = lambda v: '%7.2f%%' % (100 * v)
    title = 'Time (%)'
  elif metric == 'abstime':
    format = lambda v: '%8.2f' % v
    prefixes = ['', 'm', 'µ', 'n']
    scaleidx = 0
    totaltime = sum(data[0].values())
    while totaltime < 100. and scaleidx < len(prefixes)-1:
      scaleidx += 1
      totaltime *= 1000
    title = 'Time (%ss)' % prefixes[scaleidx]
    multiplier = 1000. ** scaleidx
  elif metric == 'cpi':
    format = lambda v: '%8.2f' % v
    title = 'CPI'
  else:
    raise ValueError('Unknown metric %s' % metric)

  print '%-20s' % title, '  '.join([ '%-9s' % ('Core %d' % core) for core in results.cores ])
  for label in results.labels:
    print '%-15s ' % label,
    for core in results.cores:
      print ' ', format(multiplier * data[core][label]),
      total[core] += data[core][label]
    print
  print
  print '%-15s ' % 'total',
  for core in results.cores:
    print ' ', format(multiplier * total[core]),
  print


def output_cpistack_gnuplot(results, metric = 'time', outputfile = 'cpi-stack', outputdir = '.', title = '', size = (640, 480), save_gnuplot_input = False):
  # Use Gnuplot to make stacked bargraphs of these cpi-stacks
  plot_data = results.get_data(metric)
  colors = results.get_colors()
  plot_labels_with_color = [ (label, 'rgb "#%02x%02x%02x"' % colors[label]) for label in results.labels ]
  gnuplot.make_stacked_bargraph(os.path.join(outputdir, outputfile), plot_labels_with_color, plot_data, size = size, title = title,
    ylabel = metric == 'cpi' and 'Cycles per instruction' or (metric == 'abstime' and 'Time (seconds)' or 'Fraction of time'), save_gnuplot_input = save_gnuplot_input)


# Legacy cpistack.cpistack() function, does most of the old one as not to break most of the scripts in papers/
def cpistack(jobid = 0, resultsdir = '.', data = None, partial = None, outputfile = 'cpi-stack', outputdir = '.',
             use_cpi = False, use_abstime = False, use_roi = True,
             use_simple = False, use_simple_mem = True, no_collapse = False,
             gen_text_stack = True, gen_plot_stack = True, gen_csv_stack = False, csv_print_header = False,
             job_name = '', title = '', threads = None, threads_mincomp = .5, return_data = False, aggregate = False,
             size = (640, 480)):

  if gen_csv_stack or csv_print_header or return_data:
    # Not all functionality has been forward-ported, preferably use cpistack_compute instead
    raise NotImplementedError

  results = cpistack_compute(jobid = jobid, resultsdir = resultsdir, data = data, partial = partial, aggregate = aggregate,
                             cores_list = threads, core_mincomp = threads_mincomp,
                             groups = use_simple and cpistack_items.build_grouplist(legacy = True) or None,
                             use_simple = use_simple, use_simple_mem = use_simple_mem, no_collapse = no_collapse)

  if gen_text_stack:
    output_cpistack_text(results)
  if gen_plot_stack:
    metric = 'cpi' if use_cpi else ('abstime' if use_abstime else 'time')
    output_cpistack_gnuplot(results = results, metric = metric, outputfile = os.path.join(outputdir, outputfile), title = title, size = size)


if __name__ == '__main__':
  def usage():
    print 'Usage:', sys.argv[0], '[-h|--help (help)] [-j <jobid> | -d <resultsdir (default: .)>] [-o <output-filename (cpi-stack)>] [--title=""] [--without-roi] [--simplified] [--no-collapse] [--no-simple-mem] [--time|--cpi|--abstime (default: time)] [--aggregate]'

  jobid = 0
  resultsdir = '.'
  partial = None
  outputfile = 'cpi-stack'
  title = ''
  metric = None
  use_simple = False
  use_simple_mem = True
  no_collapse = False
  aggregate = False
  save_gnuplot_input = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hj:d:o:", [ "help", "title=", "no-roi", "simplified", "no-collapse", "no-simple-mem", "cpi", "time", "abstime", "aggregate", "partial=", "save-gnuplot-input" ])
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
      metric = 'time'
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
    if o == '--save-gnuplot-input':
      save_gnuplot_input = True

  if args:
    usage()
    sys.exit(-1)

  results = cpistack_compute(jobid = jobid, resultsdir = resultsdir, partial = partial, aggregate = aggregate,
                             groups = use_simple and cpistack_items.build_grouplist(legacy = True) or None,
                             use_simple = use_simple, use_simple_mem = use_simple_mem, no_collapse = no_collapse)

  if len(results.cores) > 1:
    output_cpistack_table(results, metric = metric or 'cpi')
  else:
    output_cpistack_text(results)
  output_cpistack_gnuplot(results = results, metric = metric or 'time', outputfile = outputfile, title = title, save_gnuplot_input = save_gnuplot_input)
