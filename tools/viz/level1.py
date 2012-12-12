# coding: utf8

import sys, os, sniper_lib, json

def format_number(n, suffixes = ' kMBT', decimals = '321', separator = ''):
  if n < 10000:
    return '%d' % n
  idx = 0
  while n >= 1000:
    n /= 1000.
    idx += 1
    if idx == len(suffixes)-1: break
  suffix = separator + suffixes[idx]
  if n >= 100:
    return ('%%.%df%%s' % int(decimals[2])) % (n, suffix)
  elif n >= 10:
    return ('%%.%df%%s' % int(decimals[1])) % (n, suffix)
  else:
    return ('%%.%df%%s' % int(decimals[0])) % (n, suffix)

def createJSONData(resultsdir, outputdir, verbose = False):
  try:
    res = sniper_lib.get_results(resultsdir = resultsdir)
  except:
    return

  results = res['results']
  config = res['config']
  ncores = int(config['general/total_cores'])
  if 'fs_to_cycles_cores' in results:
    cycles_scale = results['fs_to_cycles_cores'][0]
  else:
    cycles_scale = 1.

  if 'barrier.global_time_begin' in results:
    # Most accurate: ask the barrier
    time_begin = results['barrier.global_time_begin'][0]
    time_end = results['barrier.global_time_end'][0]
  elif 'performance_model.elapsed_time_end' in self.stats:
    # Guess based on core that has the latest time (future wakeup is less common than sleep on futex)
    time_begin = max(results['performance_model.elapsed_time_begin'])
    time_end = max(results['performance_model.elapsed_time_end'])
  ninstrs = sum(results['performance_model.instruction_count'])
  cycles = cycles_scale * (time_end - time_begin)

  def format_mpki(value):
    return '%.3f' % (1000. * value / float(ninstrs))

  data = {
    'ncores': ncores,
    'ninstrs': format_number(ninstrs),
    'time': format_number(time_end - time_begin, suffixes = ['fs', 'ps', 'ns', 'µs', 'ms', 's']),
    'cycles': format_number(cycles),
    'ipc': '%.3f' % (ninstrs / float(cycles) / ncores),
    'branchmis': format_mpki(sum(results['branch_predictor.num-incorrect'])),
    'dram': format_mpki(sum(results['dram.reads']) + sum(results['dram.writes'])),
  }
  for cache in [ 'L1-I', 'L1-D' ] + [ 'L%u'%l for l in range(2, 5) ]:
    if '%s.loads' % cache in results:
      data['cache.%s' % cache] = format_mpki(sum(results['%s.load-misses'%cache]) + sum(results['%s.store-misses-I'%cache]))

  data['html'] = '''\
<table>
<tr><th>Cores</th><td>%(ncores)d</td>         <th>Branch MPKI</th><td>%(branchmis)s</td></tr>
<tr><th>Instructions</th><td>%(ninstrs)s</td> <th>L1-I MPKI</th><td>%(cache.L1-I)s</td></tr>
<tr><th>IPC</th><td>%(ipc)s</td>              <th>L1-D MPKI</th><td>%(cache.L1-D)s</td></tr>
<tr><th>Cycles</th><td>%(cycles)s</td>        <th>L2 MPKI</th><td>%(cache.L2)s</td></tr>
<tr><th>Time</th><td>%(time)s</td>            <th>DRAM APKI</th><td>%(dram)s</td></tr>
</table>
''' % data

  jsonfile = open(os.path.join(outputdir,'basicstats.txt'), "w")
  jsonfile.write("basicstats = "+json.dumps(data)+";\n")
  jsonfile.close()
