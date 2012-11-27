#!/usr/bin/env python

import os, sys, re, collections, gnuplot, buildstack, getopt, sniper_lib, sniper_config, operator, colorsys

try:
  collections.defaultdict()
except AttributeError, e:
  print sys.argv[0], "Error: This script requires Python version 2.5 or greater"
  sys.exit()


def getdata(jobid = '', resultsdir = '', data = None, partial = None):
  if data:
    res = data
  else:
    res = sniper_lib.get_results(jobid, resultsdir, partial = partial)
  stats = res['results']

  ncores = int(res['config']['general/total_cores'])
  instrs = stats['performance_model.instruction_count']
  try:
    times = stats['performance_model.elapsed_time']
    cycles_scale = stats['fs_to_cycles_cores']
  except KeyError:
    # On error, assume that we are using the pre-DVFS version
    times = stats['performance_model.cycle_count']
    cycles_scale = [ 1. for idx in range(ncores) ]
  # Figure out when the interval of time, represented by partial, actually begins/ends
  # Since cores can account for time in chunks, per-core time can be
  # both before (``wakeup at future time X'') or after (``sleep until woken up'')
  # the current time.
  if 'barrier.global_time_begin' in stats:
    # Most accurate: ask the barrier
    time0_begin = stats['barrier.global_time_begin'][0]
    time0_end = stats['barrier.global_time_end'][0]
  else:
    # Guess based on core that has the latest time (future wakeup is less common than sleep on futex)
    time0_begin = max(stats['performance_model.elapsed_time_begin'])
    time0_end = max(stats['performance_model.elapsed_time_end'])
  times = [ stats['performance_model.elapsed_time_end'][core] - time0_begin for core in range(ncores) ]

  if stats.get('fastforward_performance_model.fastforwarded_time', [0])[0]:
    fastforward_scale = times[0] / (times[0] - stats['fastforward_performance_model.fastforwarded_time'][0])
    times = [ t-f for t, f in zip(times, stats['fastforward_performance_model.fastforwarded_time']) ]
  else:
    fastforward_scale = 1.
  if 'performance_model.cpiFastforwardTime' in stats:
    del stats['performance_model.cpiFastforwardTime']


  data = collections.defaultdict(collections.defaultdict)
  for key, values in stats.items():
    if '.cpi' in key:
      key = key.split('.cpi')[1]
      for core in range(ncores):
        data[core][key] = values[core] * cycles_scale[core]

  if not data:
    raise ValueError('No .cpi data found, simulation did not use the interval core model')

  # Split up cpiBase into 1/issue and path dependencies
  for core in range(ncores):
    if data[core]['SyncMemAccess'] == data[core]['SyncPthreadBarrier']:
      # Work around a bug in iGraphite where SyncMemAccess wrongly copied from SyncPthreadBarrier
      # Since SyncMemAccess usually isn't very big anyway, setting it to zero should be accurate enough
      # For simulations with a fixed version of iGraphite, the changes of SyncMemAccess being identical to
      #   SyncPthreadBarrier, down to the last femtosecond, are slim, so this code shouldn't trigger
      data[core]['SyncMemAccess'] = 0
    if data[core].get('StartTime') == None:
      # Fix a bug whereby the start time was not being reported in the CPI stacks correctly
      data[core]['StartTime'] = cycles_scale * stats['performance_model.idle_elapsed_time'][core] - \
                                data[core]['SyncFutex']       - data[core]['SyncPthreadMutex']    - \
                                data[core]['SyncPthreadCond'] - data[core]['SyncPthreadBarrier']  - \
                                data[core]['Recv']
    # Critical path accounting
    cpContrMap = {
      # critical path components
      'interval_timer.cpContr_generic': 'PathInt',
      'interval_timer.cpContr_store': 'PathStore',
      'interval_timer.cpContr_load_other': 'PathLoadX',
      'interval_timer.cpContr_branch': 'PathBranch',
      'interval_timer.cpContr_load_l1': 'DataCacheL1',
      'interval_timer.cpContr_load_l2': 'DataCacheL2',
      'interval_timer.cpContr_load_l3': 'DataCacheL3',
      'interval_timer.cpContr_fp_addsub': 'PathFP',
      'interval_timer.cpContr_fp_muldiv': 'PathFP',
      # issue ports
      'interval_timer.cpContr_port0': 'PathP0',
      'interval_timer.cpContr_port1': 'PathP1',
      'interval_timer.cpContr_port2': 'PathP2',
      'interval_timer.cpContr_port34': 'PathP34',
      'interval_timer.cpContr_port5': 'PathP5',
      'interval_timer.cpContr_port05': 'PathP05',
      'interval_timer.cpContr_port015': 'PathP015',
    }
    for k in res['results']:
      if k.startswith('interval_timer.cpContr_'):
        if k not in cpContrMap.keys():
          print 'Missing in cpContrMap: ', k
    # Keep 1/width as base CPI component, break down the remainder according to critical path contributors
    BaseBest = instrs[core] / float(sniper_config.get_config(res['config'], 'perf_model/core/interval_timer/dispatch_width', core))
    BaseAct = data[core]['Base']
    BaseCp = BaseAct - BaseBest
    scale = BaseCp / (BaseAct or 1)
    for cpName, cpiName in cpContrMap.items():
      val = float(res['results'].get(cpName, [0]*ncores)[core]) / 1e6
      data[core]['Base'] -= val * scale
      data[core][cpiName] = data[core].get(cpiName, 0) + val * scale
    # Issue width
    for key, values in res['results'].items():
      if key.startswith('interval_timer.detailed-cpiBase-'):
        if 'DispatchWidth' in key:
          if 'DispatchRate' not in key: # We already accounted for DispatchRate above, don't do it twice
            data[core]['Base'] -= values[core]
            data[core]['Issue'] = data[core].get('Issue', 0) + values[core]
    # Fix up large cpiSync fractions that started before but ended inside our interval
    time0_me = stats['performance_model.elapsed_time_begin'][core]
    if time0_me < time0_begin:
      time0_extra = time0_begin - time0_me
      #    Number of cycles that weren't accounted for when starting this interval
      cycles_extra = time0_extra * cycles_scale[core]
      #    Components that could be the cause of cycles_extra. It should be just one, but if there's many, we'll have to guess
      sync_components = dict([ (key, value) for key, value in data[core].items() if (key.startswith('Sync') or key == 'StartTime') and value > cycles_extra ])
      sync_total = sum(sync_components.values())
      for key, value in sync_components.items():
        data[core][key] -= cycles_extra*value/float(sync_total)
    data[core]['Imbalance'] = cycles_scale[core] * max(times) - sum(data[core].values())

  return data, ncores, instrs, times, cycles_scale, fastforward_scale


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


def cpistack(jobid = 0, resultsdir = '.', data = None, partial = None, outputfile = 'cpi-stack', outputdir = '.',
             use_cpi = False, use_abstime = False, use_roi = True,
             use_simple = False, use_simple_mem = True, no_collapse = False,
             gen_text_stack = True, gen_plot_stack = True, gen_csv_stack = False, csv_print_header = False,
             job_name = '', title = '', threads = None, threads_mincomp = .5, return_data = False, aggregate = False,
             size = (640, 480)):

  data, ncores, instrs, times, cycles_scale, fastforward_scale = getdata(jobid = jobid, resultsdir = resultsdir, data = data, partial = partial)

  if threads:
    data   = dict([ (i, data[i]) for i in threads ])
    ncores = len(threads)
  else:
    threads = range(ncores)

  if threads_mincomp:
    compfrac = get_compfrac(data, cycles_scale[0] * max(times))
    csv_threads = [ core for core in threads if threads_mincomp < compfrac[core] ]
  else:
    csv_threads = threads

  if aggregate:
    data = { 0: dict([ (key, sum([ data[core][key] for core in csv_threads ]) / len(csv_threads)) for key in data[threads[0]].keys() ]) }
    instrs = { 0: sum(instrs[core] for core in csv_threads) / len(csv_threads) }
    threads = [0]
    csv_threads = [0]

  items, all_names, names_to_contributions = get_items(use_simple, use_simple_mem = use_simple_mem)

  results = buildstack.merge_items(data, items, nocollapse = no_collapse)


  plot_labels = []
  plot_data = {}
  max_cycles = cycles_scale[0] * max(times)

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
          plot_data[core][name] = fastforward_scale * (float(value) / cycles_scale[0]) / 1e15 # cycles to femtoseconds to seconds
        else:
          plot_data[core][name] = float(value) / max_cycles
    if gen_text_stack:
      print
      print '  %-15s    %6.2f    %6.2f%%    %6.2fs' % ('total', float(total) / (instrs[core] or 1), 100 * float(total) / scale, fastforward_scale * (float(total) / cycles_scale[0]) / 1e15)

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
    base_colors = {'compute': (0xff,0,0), 'communicate': (0,0xff,0), 'synchronize': (0,0,0xff)}
    if 'other' in plot_labels_ordered:
      all_names.append('other')
      names_to_contributions['other'] = 'other'
      base_colors['other'] = (0,0,0)
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
    plot_labels_with_color = zip(plot_labels_ordered, map(lambda x:'rgb "#%02x%02x%02x"'%get_next_color(names_to_contributions[x]),plot_labels_ordered))
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
