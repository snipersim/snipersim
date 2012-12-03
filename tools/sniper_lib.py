# A copy of this file is distributed with the binaries of Graphite and Benchmarks

import sys, os, re, sniper_stats, sniper_config
try:
  import json
except ImportError:
  import localjson as json


try:
  import intelqueue, iqclient, packdir, app_constraints
  ic = iqclient.IntelClient()
  ic_invalid = False
except ImportError:
  ic_invalid = True


class SniperResultsException(Exception): pass


def get_results(jobid = None, resultsdir = None, partial = None, force = False, metrics = None):
  if jobid:
    if ic_invalid:
      raise RuntimeError('Cannot fetch results from server, make sure BENCHMARKS_ROOT points to a valid copy of benchmarks+iqlib')
    results = ic.graphite_results(jobid, partial)
    simcfg = ic.job_output(jobid, 'sim.cfg', force)
  elif resultsdir:
    results = parse_results_from_dir(resultsdir, partial = partial, metrics = metrics)
    simcfg = file(os.path.join(resultsdir, 'sim.cfg')).read()
  else:
    raise ValueError('Need either jobid or resultsdir')

  config = sniper_config.parse_config(simcfg)
  return {
    'config': config,
    'results': stats_process(config, results),
  }


def get_name(jobid = None, resultsdir = None):
  name = None
  if jobid:
    if ic_invalid:
      raise RuntimeError('Cannot fetch results from server, make sure BENCHMARKS_ROOT points to a valid copy of benchmarks+iqlib')
    name = ic.job_stat(jobid)['name']
  elif resultsdir:
    # Create a jobname from the results directory
    name = os.path.basename(os.path.realpath(resultsdir))
  else:
    raise ValueError('Need either jobid or resultsdir')

  return {
    'name': name
  }


def stats_process(config, results):
  ncores = int(config['general/total_cores'])
  stats = {}
  for key, core, value in results:
     if core == -1:
       stats[key] = value
     else:
       if key not in stats:
         stats[key] = [0]*ncores
       if core < len(stats[key]):
         stats[key][core] = value
       else:
         nskipped = core - len(stats[key])
         stats[key] += [0]*nskipped + [value]
  # add computed stats
  try:
    l1access = sum(stats['L1-D.load-misses']) + sum(stats['L1-D.store-misses'])
    l1time = sum(stats['L1-D.total-latency'])
    stats['l1misslat'] = l1time / float(l1access or 1)
  except KeyError:
    pass
  stats['pthread_locks_contended'] = float(sum(stats.get('pthread.pthread_mutex_lock_contended', [0]))) / (sum(stats.get('pthread.pthread_mutex_lock_count', [0])) or 1)
  # femtosecond to cycles conversion
  freq = [ 1e9 * float(sniper_config.get_config(config, 'perf_model/core/frequency', idx)) for idx in range(ncores) ]
  stats['fs_to_cycles_cores'] = map(lambda f: f / 1e15, freq)
  # Backwards compatible version returning fs_to_cycles for core 0, for heterogeneous configurations fs_to_cycles_cores needs to be used
  stats['fs_to_cycles'] = stats['fs_to_cycles_cores'][0]
  # DVFS-enabled runs: emulate cycle_count asuming constant (initial) frequency
  if 'performance_model.elapsed_time' in stats and 'performance_model.cycle_count' not in stats:
    stats['performance_model.cycle_count'] = [ stats['fs_to_cycles_cores'][idx] * stats['performance_model.elapsed_time'][idx] for idx in range(ncores) ]
  # IPC
  stats['ipc'] = sum(stats.get('performance_model.instruction_count', [0])) / float(sum(stats.get('performance_model.cycle_count', [0])) or 1e16)

  return stats


def parse_results_from_dir(resultsdir, partial = None, metrics = None):
  results = []

  ## sim.cfg
  simcfg = os.path.join(resultsdir, 'sim.cfg')
  if not os.path.exists(simcfg):
    raise SniperResultsException("No valid configuration found")
  simcfg = sniper_config.parse_config(open(simcfg).read())
  ncores = int(simcfg['general/total_cores'])

  results += [ ('ncores', -1, ncores) ]
  results += [ ('corefreq', idx, 1e9 * float(sniper_config.get_config(simcfg, 'perf_model/core/frequency', idx))) for idx in range(ncores) ]

  ## stdout.txt
  walltime = 0
  roi = { 'instrs': 0, 'ipstotal': 0, 'ipscore': 0 }
  stdout = os.path.join(resultsdir, 'stdout.txt')
  stdout = os.path.exists(stdout) and open(stdout) or []
  for line in stdout:
    for marker in ('[SNIPER]', '[GRAPHITE]'):
      try:
        if line.startswith('%s Leaving ROI after' % marker):
          walltime = float(line.split()[-2])
      except (IndexError, ValueError):
        pass
      try:
        if re.match('^\[%s(:0)?\] Simulated' % marker, line):
          roi = re.match('\[%s(:0)?\] Simulated ([0-9.]+)M instructions @ ([0-9.]+) KIPS \(([0-9.]+) KIPS / target core' % marker, roi)
          roi = { 'instrs': float(roi.group(2))*1e6, 'ipstotal': float(roi.group(3))*1e3, 'ipscore': float(roi.group(4))*1e3 }
      except (IndexError, ValueError):
        pass

  results.append(('roi.walltime', -1, walltime))
  results.append(('roi.instrs', -1, roi['instrs']))
  results.append(('roi.ipstotal', -1, roi['ipstotal']))
  results.append(('roi.ipscore', -1, roi['ipscore']))

  ## sim.info or graphite.out
  siminfo = os.path.join(resultsdir, 'sim.info')
  graphiteout = os.path.join(resultsdir, 'graphite.out')
  if os.path.exists(siminfo):
    siminfo = eval(open(siminfo).read())
  elif os.path.exists(graphiteout):
    siminfo = eval(open(graphiteout).read())
  else:
    siminfo = None
  if siminfo:
    # If we're called from inside run-graphite, sim.info may not yet exist
    results.append(('walltime', -1, siminfo['t_elapsed']))
    results.append(('vmem', -1, siminfo['vmem']))

  ## sim.stats
  if partial:
    k1, k2 = partial[:2]
  else:
    k1, k2 = 'roi-begin', 'roi-end'

  stats = sniper_stats.SniperStats(resultsdir)
  results += stats.parse_stats((k1, k2), ncores, metrics = metrics)

  ## power.py
  power = {}
  powerfile = os.path.join(resultsdir, 'power.py')
  if os.path.exists(powerfile):
    exec(open(powerfile).read())
    for key, value in power.items():
      results.append(('power.%s' % key, -1, value))

  return results


def get_results_file(filename, jobid = None, resultsdir = None, force = False):
  if jobid:
    return ic.job_output(jobid, filename, force)
  else:
    filename = os.path.join(resultsdir, filename)
    if os.path.exists(filename):
      return file(filename).read()
    else:
      return None


def format_size(size, digits = 1):
  i = 0
  while size >= 1024:
    size /= 1024.
    i += 1
  return '%.*f%sB' % (digits, size, [' ', 'K', 'M', 'G', 'T', 'P', 'E'][i])


def find_children(pid):
  # build list of all children per ppid
  children = {}
  for _pid in os.listdir('/proc'):
    try:
      _pid = int(_pid)
    except ValueError:
      continue # not a pid
    try:
      stat = file('/proc/%u/stat' % _pid).read()
    except IOError:
      continue # pid already gone
    ppid = int(stat.split()[3])
    if ppid not in children:
      children[ppid] = []
    children[ppid].append(_pid)
  # recursive function to return children of a given pid
  def __find_children(ppid):
    ret = [ppid]
    if ppid in children:
      for pid in children[ppid]:
        ret.extend(__find_children(pid))
    return ret
  return __find_children(pid)


def kill_children():
  children = find_children(os.getpid())
  for pid in children:
    if pid != os.getpid():
      try: os.kill(pid, 9)
      except OSError: pass
