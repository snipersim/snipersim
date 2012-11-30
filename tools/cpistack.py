#!/usr/bin/env python

import os, sys, re, collections, gnuplot, getopt, operator, colorsys
import sniper_lib, sniper_config, cpistack_data, cpistack_items, cpistack_results

try:
  collections.defaultdict()
except AttributeError, e:
  print sys.argv[0], "Error: This script requires Python version 2.5 or greater"
  sys.exit()


def color_tint_shade(base_color, num):
  base_color = map(lambda x:float(x)/255, base_color)
  base_color = colorsys.rgb_to_hsv(*base_color)
  colors = []
  delta = 0.6 / float((num/2) or 1)
  shade = 1.0
  for _ in range(num/2):
    shade -= delta
    colors.append((base_color[0],base_color[1],shade))
  colors = colors[::-1] # Reverse
  if num % 2 == 1:
    colors.append(base_color)
  tint = 1.0
  for _ in range(num/2):
    tint -= delta
    colors.append((base_color[0],tint,base_color[2]))
  colors = map(lambda x:colorsys.hsv_to_rgb(*x),colors)
  colors = map(lambda x:tuple(map(lambda y:int(y*255),x)),colors)
  return colors


def get_colors(plot_labels_ordered, cpiitems):
    contribution_counts = collections.defaultdict(int)
    for i in plot_labels_ordered:
      contribution_counts[cpiitems.names_to_contributions[i]] += 1
    color_ranges = {}
    next_color_index = {}
    for name, color, _ in cpiitems.groups:
      color_ranges[name] = color_tint_shade(color, contribution_counts[name])
      next_color_index[name] = 0
    def get_next_color(contr):
      idx = next_color_index[contr]
      next_color_index[contr] += 1
      return color_ranges[contr][idx]
    return map(lambda x:get_next_color(cpiitems.names_to_contributions[x]),plot_labels_ordered)


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
  labels, cores, cpi = results.get_data('cpi')
  labels, cores, time = results.get_data('time')

  for core in cores:
    if len(cores) > 1:
      print 'Core', core
    total = { 'cpi': 0, 'time': 0 }
    for label in labels:
      print '  %-15s    %6.2f    %6.2f%%' % (label, cpi[core][label], 100 * time[core][label])
      total['cpi'] += cpi[core][label]
      total['time'] += time[core][label]
    print
    print '  %-15s    %6.2f    %6.2f%%' % ('total', total['cpi'], total['time'])


def output_cpistack_gnuplot(results, metric = 'time', outputfile = 'cpi-stack', outputdir = '.', size = (640, 480)):
  plot_labels, plot_cores, plot_data = results.get_data(metric)
  # Use Gnuplot to make stacked bargraphs of these cpi-stacks
  plot_labels_with_color = zip(plot_labels, map(lambda x:'rgb "#%02x%02x%02x"'%x,get_colors(plot_labels, results.cpiitems)))
  gnuplot.make_stacked_bargraph(os.path.join(outputdir, outputfile), plot_labels_with_color, plot_data, size = size, title = title,
    ylabel = metric == 'cpi' and 'Cycles per instruction' or (metric == 'abstime' and 'Time (seconds)' or 'Fraction of time'))


# Legacy function doing everything
def cpistack(jobid = 0, resultsdir = '.', data = None, partial = None, outputfile = 'cpi-stack', outputdir = '.',
             metric = 'time', use_roi = True,
             use_simple = False, use_simple_mem = True, no_collapse = False,
             gen_text_stack = True, gen_plot_stack = True,
             job_name = '', title = '', threads = None, threads_mincomp = 0., aggregate = False,
             size = (640, 480)):

  results = cpistack_compute(jobid = jobid, resultsdir = resultsdir, data = data, partial = partial,
                             cores_list = threads, core_mincomp = threads_mincomp, aggregate = aggregate,
                             groups = use_simple and cpistack_items.build_grouplist(legacy = True) or None,
                             use_simple = use_simple, use_simple_mem = use_simple_mem, no_collapse = no_collapse)

  plot_labels, plot_cores, plot_data = results.get_data(metric)


  if gen_text_stack:
    output_cpistack_text(results)

  if gen_plot_stack:
    output_cpistack_gnuplot(results, metric, outputfile, outputdir, size)

  return plot_labels, plot_cores, plot_data


if __name__ == '__main__':
  def usage():
    print 'Usage:', sys.argv[0], '[-h|--help (help)] [-j <jobid> | -d <resultsdir (default: .)>] [-o <output-filename (cpi-stack)>] [--title=""] [--without-roi] [--simplified] [--no-collapse] [--no-simple-mem] [--time|--cpi|--abstime (default: time)] [--aggregate]'

  jobid = 0
  resultsdir = '.'
  partial = None
  outputfile = 'cpi-stack'
  title = ''
  metric = 'time'
  use_roi = True
  use_simple = False
  use_simple_mem = True
  no_collapse = False
  aggregate = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hj:d:o:", [ "help", "title=", "without-roi", "simplified", "no-collapse", "no-simple-mem", "cpi", "time", "abstime", "aggregate", "partial=" ])
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
    if o == '--without-roi':
      use_roi = False
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

  cpistack(
    jobid = jobid,
    resultsdir = resultsdir,
    partial = partial,
    outputfile = outputfile,
    title = title,
    metric = metric,
    use_roi = use_roi,
    use_simple = use_simple,
    use_simple_mem = use_simple_mem,
    no_collapse = no_collapse,
    aggregate = aggregate)
