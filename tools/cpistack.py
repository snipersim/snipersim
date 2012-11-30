#!/usr/bin/env python

import os, sys, re, collections, gnuplot, buildstack, getopt, operator, colorsys
import sniper_lib, sniper_config, cpistack_data

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


def get_items(use_simple = False, use_simple_sync = False, use_simple_mem = True):
  # List of all CPI contributors: <title>, <threshold (%)>, <contributors>
  # <contributors> can be string: key name in sim.stats (sans "roi-end.*[<corenum>].cpi")
  #                       list  : recursive list of sub-contributors
  #                       tuple : list of key names that are summed anonymously

  all_items = [
    [ 'dispatch_width', .01,   'Issue' ],
    [ 'base',           .01,   'Base' ],
    [ 'depend',   .01,   [
      [ 'int',      .01, 'PathInt' ],
      [ 'fp',       .01, 'PathFP' ],
      [ 'branch',   .01, 'PathBranch' ],
    ] ],
    [ 'issue',    .01, [
      [ 'port0',        .01,    'PathP0' ],
      [ 'port1',        .01,    'PathP1' ],
      [ 'port2',       .01,    'PathP2' ],
      [ 'port34',        .01,    'PathP34' ],
      [ 'port5',        .01,    'PathP5' ],
      [ 'port05',      .01,    'PathP05' ],
      [ 'port015',      .01,    'PathP015' ],
    ] ],
    [ 'branch',   .01, 'BranchPredictor' ],
    [ 'serial',   .01, ('Serialization', 'LongLatency') ], # FIXME: can LongLatency be anything other than MFENCE?
    [ 'smt',            .01,   'SMT' ],
    [ 'itlb',     .01, 'ITLBMiss' ],
    [ 'dtlb',     .01, 'DTLBMiss' ],
    [ 'ifetch',   .01, (
          'DataCacheL1I', 'InstructionCacheL1I', 'InstructionCacheL1', 'InstructionCacheL1_S',
          'InstructionCacheL2', 'InstructionCacheL2_S', 'InstructionCacheL3', 'InstructionCacheL3_S',
          'InstructionCacheL4',  'InstructionCacheL4_S', 'InstructionCachemiss', 'InstructionCache????',
          'InstructionCachedram-cache', 'InstructionCachedram',
          'InstructionCachedram-remote', 'InstructionCachecache-remote', 'InstructionCachedram-local',
          'InstructionCachepredicate-false', 'InstructionCacheunknown') ],
  ]
  if use_simple_mem:
    all_items += [
    [ 'mem',      .01, [
      [ 'l1d',      .01, ('DataCacheL1', 'DataCacheL1_S', 'PathLoadX', 'PathStore') ],
      [ 'l2',       .01, ('DataCacheL2', 'DataCacheL2_S') ],
      [ 'l3',       .01, ('DataCacheL3', 'DataCacheL3_S') ],
      [ 'l4',       .01, ('DataCacheL4', 'DataCacheL4_S') ],
      [ 'remote',   .01, 'DataCachecache-remote' ],
      [ 'dram-cache', .01, 'DataCachedram-cache' ],
      [ 'dram',     .01, ('DataCachedram', 'DataCachedram-local', 'DataCachedram-remote', 'DataCachemiss', 'DataCache????', 'DataCachepredicate-false', 'DataCacheunknown') ],
    ] ],
  ]
  else:
    all_items += [
    [ 'mem',      .01, [
      [ 'l0d_neighbor', .01, 'DataCacheL1_S' ],
      [ 'l1d',          .01, ('DataCacheL1', 'PathLoadX', 'PathStore') ],
      [ 'l1_neighbor',  .01, 'DataCacheL2_S' ],
      [ 'l2',           .01, 'DataCacheL2' ],
      [ 'l2_neighbor',  .01, 'DataCacheL3_S' ],
      [ 'l3',           .01, 'DataCacheL3' ],
      [ 'l3_neighbor',  .01, 'DataCacheL4_S' ],
      [ 'l4',           .01, 'DataCacheL4' ],
      [ 'off_socket',   .01, 'DataCachecache-remote' ],
      [ 'dram',         .01, ('DataCachedram-local', 'DataCachedram-remote', 'DataCachemiss', 'DataCache????', 'DataCachepredicate-false', 'DataCacheunknown') ],
    ] ],
  ]

  if use_simple_sync:
    all_items += [ [ 'sync', .01, ('SyncFutex', 'SyncPthreadMutex', 'SyncPthreadCond', 'SyncPthreadBarrier', 'SyncJoin',
                                   'SyncPause', 'SyncSleep', 'SyncUnscheduled', 'SyncMemAccess', 'Recv' ) ] ]
  else:
    all_items += [
    [ 'sync',     .01, [
      [ 'futex',    .01, 'SyncFutex' ],
      [ 'mutex',    .01, 'SyncPthreadMutex' ],
      [ 'cond',     .01, 'SyncPthreadCond' ],
      [ 'barrier',  .01, 'SyncPthreadBarrier' ],
      [ 'join',     .01, 'SyncJoin' ],
      [ 'pause',    .01, 'SyncPause' ],
      [ 'sleep',    .01, 'SyncSleep' ],
      [ 'unscheduled', .01, 'SyncUnscheduled' ],
      [ 'memaccess',.01, 'SyncMemAccess' ],
      [ 'recv',     .01, 'Recv' ],
    ] ],
  ]

  all_items += [
    [ 'dvfs-transition', 0.01, 'SyncDvfsTransition' ],
    [ 'imbalance', 0.01, [
      [ 'start', 0.01, 'StartTime' ],
      [ 'end',   0.01, 'Imbalance' ],
    ] ],
  ]

  def _findall(items, keys = None):
    res = []
    for name, threshold, key_or_items in items:
      if not keys or name in keys:
        if type(key_or_items) is list:
          res += _findall(key_or_items)
        elif type(key_or_items) is tuple:
          res += list(key_or_items)
        else:
          res += [ key_or_items ]
    return res
  def findall(*keys): return tuple(_findall(all_items, keys))

  simple_groups = (
    ('compute', ('dispatch_width', 'base', 'issue', 'depend',
                'branch', 'serial', 'smt')),
    ('communicate', ('itlb','dtlb','ifetch','mem',)),
    ('synchronize', ('sync', 'recv', 'dvfs-transition', 'imbalance')),
  )

  if use_simple:
    new_all_items = []
    new_simple_groups = []
    for k,v in simple_groups:
      new_all_items.append([k, 0, findall(*v)])
      new_simple_groups.append((k,(k,)))
    all_items = new_all_items
    simple_groups = new_simple_groups

  all_names = buildstack.get_names('', all_items)

  base_contribution = {}
  for group, members in simple_groups:
    for name in buildstack.get_names('', all_items, True, members):
      base_contribution[name] = group

  return all_items, all_names, base_contribution


def get_compfrac(data, max_time):
  return dict([ (
    core,
    1 - (data[core].get('StartTime', 0) + data[core].get('Imbalance', 0) + data[core].get('SyncPthreadCond', 0) + \
         data[core].get('SyncPthreadBarrier', 0) + data[core].get('SyncJoin', 0) + data[core].get('Recv', 0)) / (float(max_time) or 1.)
  ) for core in data.keys() ])


def get_colors(plot_labels_ordered,
               names_to_contributions,
               base_colors = {'compute': (0xff,0,0), 'communicate': (0,0xff,0), 'synchronize': (0,0,0xff), 'other': (0,0,0)}):
    contribution_counts = collections.defaultdict(int)
    for i in plot_labels_ordered:
      contribution_counts[names_to_contributions[i]] += 1
    color_ranges = {}
    next_color_index = {}
    for b in base_colors.iterkeys():
      color_ranges[b] = color_tint_shade(base_colors[b], contribution_counts[b])
      next_color_index[b] = 0
    def get_next_color(contr):
      idx = next_color_index[contr]
      next_color_index[contr] += 1
      return color_ranges[contr][idx]
    return map(lambda x:get_next_color(names_to_contributions[x]),plot_labels_ordered)


def cpistack(jobid = 0, resultsdir = '.', data = None, partial = None, outputfile = 'cpi-stack', outputdir = '.',
             use_cpi = False, use_abstime = False, use_roi = True,
             use_simple = False, use_simple_mem = True, no_collapse = False,
             gen_text_stack = True, gen_plot_stack = True, gen_csv_stack = False, csv_print_header = False,
             job_name = '', title = '', threads = None, threads_mincomp = .5, return_data = False, aggregate = False,
             size = (640, 480)):

  cpidata = cpistack_data.CpiData(jobid = jobid, resultsdir = resultsdir, data = data, partial = partial)

  if threads:
    data   = dict([ (i, cpidata.data[i]) for i in threads ])
    ncores = len(threads)
  else:
    data = cpidata.data
    threads = range(cpidata.ncores)
    ncores = cpidata.ncores

  if threads_mincomp:
    compfrac = get_compfrac(data, cpidata.cycles_scale[0] * max(cpidata.times))
    csv_threads = [ core for core in threads if threads_mincomp < compfrac[core] ]
  else:
    csv_threads = threads

  if aggregate:
    data = { 0: dict([ (key, sum([ data[core][key] for core in csv_threads ]) / len(csv_threads)) for key in data[threads[0]].keys() ]) }
    instrs = { 0: sum(cpidata.instrs[core] for core in csv_threads) / len(csv_threads) }
    threads = [0]
    csv_threads = [0]
  else:
    instrs = cpidata.instrs

  items, all_names, names_to_contributions = get_items(use_simple, use_simple_mem = use_simple_mem)

  results = buildstack.merge_items(data, items, nocollapse = no_collapse)


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
        print '  %-15s    %6.2f    %6.2f%%    %6.2f%%' % (name, float(value) / (instrs[core] or 1), 100 * float(value) / scale, 100 * float(value) / max_cycles)
      total += value
      if gen_plot_stack or return_data:
        plot_labels.append(name)
        if use_cpi:
          plot_data[core][name] = float(value) / (instrs[core] or 1)
        elif use_abstime:
          plot_data[core][name] = cpidata.fastforward_scale * (float(value) / cpidata.cycles_scale[0]) / 1e15 # cycles to femtoseconds to seconds
        else:
          plot_data[core][name] = float(value) / max_cycles
    if gen_text_stack:
      print
      print '  %-15s    %6.2f    %6.2f%%    %6.2fs' % ('total', float(total) / (instrs[core] or 1), 100 * float(total) / scale, cpidata.fastforward_scale * (float(total) / cpidata.cycles_scale[0]) / 1e15)

  # First, create an ordered list of labels that is the superset of all labels used from all cores
  # Then remove items that are not used, creating an ordered list with all currently used labels
  plot_labels_ordered = all_names[:] + ['other']
  for label in plot_labels_ordered[:]:
    if label not in plot_labels:
      plot_labels_ordered.remove(label)
    else:
      # If this is a valid label, make sure that it exists in all plot_data entries
      for core in threads:
        plot_data[core].setdefault(label, 0.0)

  # Create CSV data
  # Take a snapshot of the data from the last core and create a CSV
  if gen_csv_stack and csv_threads:
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
      values = [ plot_data[core][label] for core in csv_threads ]
      f.write(',%f' % (sum(values) / float(len(values))))
    f.write('\n')
    f.close()

  # Use Gnuplot to make stacked bargraphs of these cpi-stacks
  if gen_plot_stack:
    if 'other' in plot_labels_ordered:
      all_names.append('other')
      names_to_contributions['other'] = 'other'
    plot_labels_with_color = zip(plot_labels_ordered, map(lambda x:'rgb "#%02x%02x%02x"'%x,get_colors(plot_labels_ordered,names_to_contributions)))
    gnuplot.make_stacked_bargraph(os.path.join(outputdir, outputfile), plot_labels_with_color, plot_data, size = size, title = title,
      ylabel = use_cpi and 'Cycles per instruction' or (use_abstime and 'Time (seconds)' or 'Fraction of time'))

  # Return cpi data if requested
  if return_data and csv_threads:
    # Create a view of the data, removing threads that do not contribute
    data_to_return = {}
    for core in csv_threads:
      data_to_return[core] = {}
      for label in plot_labels_ordered:
        data_to_return[core][label] = plot_data[core][label]
    return plot_labels_ordered, csv_threads, data_to_return


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
