#!/usr/bin/env python

import os, sys, re, collections, gnuplot, buildstack, getopt, operator, colorsys
import sniper_lib, sniper_config, cpistack_data, cpistack_items

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


def cpistack(jobid = 0, resultsdir = '.', data = None, partial = None, outputfile = 'cpi-stack', outputdir = '.',
             use_cpi = False, use_abstime = False, use_roi = True,
             use_simple = False, use_simple_mem = True, no_collapse = False,
             gen_text_stack = True, gen_plot_stack = True, gen_csv_stack = False, csv_print_header = False,
             job_name = '', title = '', threads = None, threads_mincomp = 0., return_data = False, aggregate = False,
             size = (640, 480)):

  cpidata = cpistack_data.CpiData(jobid = jobid, resultsdir = resultsdir, data = data, partial = partial)
  cpidata.filter(cores_list = threads, core_mincomp = threads_mincomp)
  if aggregate:
    cpidata.aggregate()

  cpiitems = cpistack_items.CpiItems(use_simple = use_simple, use_simple_mem = use_simple_mem)

  results = buildstack.merge_items(cpidata.data, cpiitems.items, nocollapse = no_collapse)


  plot_labels = []
  plot_data = {}
  max_cycles = cpidata.cycles_scale[0] * max(cpidata.times)

  if not max_cycles:
    raise ValueError("No cycles accounted during interval")

  if gen_text_stack: print '                     CPI      CPI %     Time %'
  for core, (res, total, other, scale) in results.items():
    if gen_text_stack and not aggregate: print 'Core', core
    plot_data[core] = {}
    total = 0
    for name, value in res:
      if gen_text_stack:
        print '  %-15s    %6.2f    %6.2f%%    %6.2f%%' % (name, float(value) / (cpidata.instrs[core] or 1), 100 * float(value) / scale, 100 * float(value) / max_cycles)
      total += value
      if gen_plot_stack or return_data:
        plot_labels.append(name)
        if use_cpi:
          plot_data[core][name] = float(value) / (cpidata.instrs[core] or 1)
        elif use_abstime:
          plot_data[core][name] = cpidata.fastforward_scale * (float(value) / cpidata.cycles_scale[0]) / 1e15 # cycles to femtoseconds to seconds
        else:
          plot_data[core][name] = float(value) / max_cycles
    if gen_text_stack:
      print
      print '  %-15s    %6.2f    %6.2f%%    %6.2fs' % ('total', float(total) / (cpidata.instrs[core] or 1), 100 * float(total) / scale, cpidata.fastforward_scale * (float(total) / cpidata.cycles_scale[0]) / 1e15)

  # First, create an ordered list of labels that is the superset of all labels used from all cores
  # Then remove items that are not used, creating an ordered list with all currently used labels
  plot_labels_ordered = cpiitems.names[:] + ['other']
  for label in plot_labels_ordered[:]:
    if label not in plot_labels:
      plot_labels_ordered.remove(label)
    else:
      # If this is a valid label, make sure that it exists in all plot_data entries
      for core in cpidata.cores:
        plot_data[core].setdefault(label, 0.0)

  # Create CSV data
  # Take a snapshot of the data from the last core and create a CSV
  if gen_csv_stack:
    f = open(os.path.join(outputdir, 'cpi-stack.csv'), "a")
    # Print the CSV header if requested
    if csv_print_header:
      f.write('name')
      for label in plot_labels_ordered:
        f.write(',' + label)
      f.write('\n')
    # Print a row of data
    csv_first = True
    if job_name:
      f.write(job_name)
    for label in plot_labels_ordered:
      values = [ plot_data[core][label] for core in cpidata.data ]
      f.write(',%f' % (sum(values) / float(len(values))))
    f.write('\n')
    f.close()

  # Use Gnuplot to make stacked bargraphs of these cpi-stacks
  if gen_plot_stack:
    if 'other' in plot_labels_ordered:
      cpiitems.add_other()
    plot_labels_with_color = zip(plot_labels_ordered, map(lambda x:'rgb "#%02x%02x%02x"'%x,get_colors(plot_labels_ordered, cpiitems)))
    gnuplot.make_stacked_bargraph(os.path.join(outputdir, outputfile), plot_labels_with_color, plot_data, size = size, title = title,
      ylabel = use_cpi and 'Cycles per instruction' or (use_abstime and 'Time (seconds)' or 'Fraction of time'))

  # Return cpi data if requested
  if return_data:
    # Create a view of the data, removing threads that do not contribute
    data_to_return = {}
    for core in cpidata.cores:
      data_to_return[core] = {}
      for label in plot_labels_ordered:
        data_to_return[core][label] = plot_data[core][label]
    return plot_labels_ordered, cpidata.cores, data_to_return


if __name__ == '__main__':
  def usage():
    print 'Usage:', sys.argv[0], '[-h|--help (help)] [-j <jobid> | -d <resultsdir (default: .)>] [-o <output-filename (cpi-stack)>] [--title=""] [--without-roi] [--simplified] [--no-collapse] [--no-simple-mem] [--time|--cpi|--abstime (default: time)] [--aggregate]'

  jobid = 0
  resultsdir = '.'
  partial = None
  outputfile = 'cpi-stack'
  title = ''
  use_cpi = False
  use_abstime = False
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
      use_cpi = True
    if o == '--abstime':
      use_abstime = True
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
    use_cpi = use_cpi,
    use_abstime = use_abstime,
    use_roi = use_roi,
    use_simple = use_simple,
    use_simple_mem = use_simple_mem,
    no_collapse = no_collapse,
    aggregate = aggregate)
