import os, sys, json, collections
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend( [os.path.abspath(os.path.join(HOME, '..'))] )
import gen_topology, sniper_lib, sniper_config, sniper_stats


# From http://stackoverflow.com/questions/600268/mkdir-p-functionality-in-python
def mkdir_p(path):
  import errno
  try:
    os.makedirs(path)
  except OSError, exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else: raise


def createJSONData(interval, num_intervals, resultsdir, outputdir, verbose = False):
  topodir = os.path.join(outputdir,'levels','topology')
  mkdir_p(topodir)

  gen_topology.gen_topology(resultsdir = resultsdir, outputobj = file(os.path.join(topodir, 'topo.svg'), 'w'), format = 'svg', embedded = True)

  config = sniper_config.parse_config(file(os.path.join(resultsdir, 'sim.cfg')).read())
  ncores = int(config['general/total_cores'])
  stats = sniper_stats.SniperStats(resultsdir)

  ids = collections.defaultdict(lambda: {})
  for name, lid, mid in stats.get_topology():
    ids[name][int(lid)] = int(mid)

  caches = [ 'L1-I', 'L1-D', 'L2', 'L3', 'L4', 'dram-cache' ]
  items = sum([ [ '%s-%d' % (name, core) for name in ['core','dram-cntlr']+caches ] for core in range(ncores) ], [])
  data = dict([ (item, {'info':'', 'sparkdata':[]}) for item in items ])
  dramcntlrs = [ lid for (name, lid, mid) in stats.get_topology() if name == 'dram-cntlr' ]


  for i in range(num_intervals):
    results = sniper_lib.get_results(config = config, stats = stats, partial = ('periodic-'+str(i*interval), 'periodic-'+str((i+1)*interval)))['results']
    if 'barrier.global_time_begin' in results:
      # Most accurate: ask the barrier
      results['time_begin'] = results['barrier.global_time_begin'][0]
      results['time_end'] = results['barrier.global_time_end'][0]
    elif 'performance_model.elapsed_time_end' in results:
      # Guess based on core that has the latest time (future wakeup is less common than sleep on futex)
      results['time_begin'] = max(results['performance_model.elapsed_time_begin'])
      results['time_end'] = max(results['performance_model.elapsed_time_end'])
    else:
      raise ValueError('Need either performance_model.elapsed_time or barrier.global_time, simulation is probably too old')

    for core in range(ncores):
      if 'fs_to_cycles_cores' in results:
        cycles_scale = results['fs_to_cycles_cores'][core]
      else:
        cycles_scale = 1.
      cycles = cycles_scale * (results['time_end'] - results['time_begin'])
      ninstrs = results['performance_model.instruction_count'][core]
      data['core-%d' % core]['sparkdata'].append('%.3f' % (ninstrs / cycles))
      data['core-%d' % core]['info'] = 'IPC (core-%d)' % core
      for cache in caches:
        if cache not in ids:
          # Cache level does not exist
          continue
        if ids[cache][core] != core:
          # Non-master cache
          continue
        if '%s.loads' % cache in results:
          # Sum misses and instruction counts over all cores sharing this cache
          misses = 0; ninstrs = 0
          for _core in range(ncores):
            if ids[cache][_core] == ids[cache][core]:
              misses += results['%s.load-misses'%cache][_core] + results['%s.store-misses-I'%cache][_core]
              ninstrs += results['performance_model.instruction_count'][_core]
          data['%s-%d' % (cache, core)]['sparkdata'].append('%.3f' % (1000. * misses / float(ninstrs or 1.)))
          data['%s-%d' % (cache, core)]['info'] = 'MPKI (%s-%d)' % (cache, core)

    for dramcntlr in dramcntlrs:
      ninstrs = sum(results['performance_model.instruction_count'])
      if ninstrs == 0:
        data['dram-cntlr-%d' % dramcntlr]['sparkdata'].append(0.); # FIXME ninstrs should not be zero while we are accessing dram
      else:
        data['dram-cntlr-%d' % dramcntlr]['sparkdata'].append('%.3f' % (1000. * (results['dram.reads'][dramcntlr] + results['dram.writes'][dramcntlr]) / (ninstrs or 1.)))
      data['dram-cntlr-%d' % dramcntlr]['info'] = 'APKI (dram-cntlr-%d)' % dramcntlr

  jsonfile = open(os.path.join(topodir, 'topology.txt'), "w")
  jsonfile.write('topology = %s' % json.dumps(data))
  jsonfile.close()
