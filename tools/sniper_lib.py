# A copy of this file is distributed with the binaries of Graphite and Benchmarks

import sys, os, time, re, tempfile, timeout, traceback, collections
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


def get_results(jobid = None, resultsdir = None, partial = None, force = False):
  if jobid:
    if ic_invalid:
      raise RuntimeError('Cannot fetch results from server, make sure BENCHMARKS_ROOT points to a valid copy of benchmarks+iqlib')
    results = ic.graphite_results(jobid, partial)
    simcfg = ic.job_output(jobid, 'sim.cfg', force)
  elif resultsdir:
    results = parse_results_from_dir(resultsdir, partial = partial)
    simcfg = file(os.path.join(resultsdir, 'sim.cfg')).read()
  else:
    raise ValueError('Need either jobid or resultsdir')

  config = parse_config(simcfg)
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
  freq = [ 1e9 * float(get_config(config, 'perf_model/core/frequency', idx)) for idx in range(ncores) ]
  stats['fs_to_cycles_cores'] = map(lambda f: f / 1e15, freq)
  # Backwards compatible version returning fs_to_cycles for core 0, for heterogeneous configurations fs_to_cycles_cores needs to be used
  stats['fs_to_cycles'] = stats['fs_to_cycles_cores'][0]
  # DVFS-enabled runs: emulate cycle_count asuming constant (initial) frequency
  if 'performance_model.elapsed_time' in stats and 'performance_model.cycle_count' not in stats:
    stats['performance_model.cycle_count'] = [ stats['fs_to_cycles_cores'][idx] * stats['performance_model.elapsed_time'][idx] for idx in range(ncores) ]
  # IPC
  stats['ipc'] = sum(stats.get('performance_model.instruction_count', [0])) / float(sum(stats.get('performance_model.cycle_count', [0])) or 1e16)

  return stats


class DefaultValue:
  def __init__(self, value):
    self.val = value
  def __call__(self):
    return self.val

# Parse sim.cfg, read from file or from ic.job_output(jobid, 'sim.cfg'), into a dictionary
def parse_config(simcfg):
  import ConfigParser, cStringIO
  cp = ConfigParser.ConfigParser()
  cp.readfp(cStringIO.StringIO(str(simcfg)))
  cfg = {}
  for section in cp.sections():
    for key, value in sorted(cp.items(section)):
      # Run through items sorted by key, so the default comes before the array one
      # Then cut off the [] array markers as they are only used to prevent duplicate option names which ConfigParser doesn't handle
      if key.endswith('[]'):
        key = key[:-2]
      if len(value) > 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
      key = '/'.join((section, key))
      if key in cfg:
        defval = cfg[key]
        cfg[key] = collections.defaultdict(DefaultValue(defval))
        for i, v in enumerate(value.split(',')):
          if v: # Only fill in entries that have been provided
            cfg[key][i] = v
      else: # If there has not been a default value provided, require all array data be populated
        if ',' in value:
          cfg[key] = []
          for i, v in enumerate(value.split(',')):
            cfg[key].append(v)
        else:
          cfg[key] = value
  return cfg


def get_config(config, key, index = None):
  is_hetero = (type(config[key]) == collections.defaultdict)
  if index is None:
    if is_hetero:
      return config[key].default_factory()
    else:
      return config[key]
  elif is_hetero:
    return config[key][index]
  else:
    return config[key]


def get_config_default(config, key, defaultval, index = None):
  if key in config:
    return get_config(config, key, index)
  else:
    return defaultval


def parse_results_from_fileobjs((simstats, simstatsbase, simstatsdelta, simout, simcfg, stdout, graphiteout, powerpy), partial = None):
  results = []

  ## sim.cfg
  if not simcfg:
    raise SniperResultsException("No valid configuration found")
  simcfg = parse_config(simcfg.read())
  ncores = int(simcfg['general/total_cores'])

  results += [ ('ncores', -1, ncores) ]
  results += [ ('corefreq', idx, 1e9 * float(get_config(simcfg, 'perf_model/core/frequency', idx))) for idx in range(ncores) ]

  ## stdout.txt
  walltime = 0
  roi = { 'instrs': 0, 'ipstotal': 0, 'ipscore': 0 }
  for line in (stdout or []):
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

  ## graphite.out
  if graphiteout:
    # If we're called from inside run-graphite, graphite.out may not yet exist
    graphiteout = eval(graphiteout.read())
    results.append(('walltime', -1, graphiteout['t_elapsed']))
    results.append(('vmem', -1, graphiteout['vmem']))

  ## sim.stats
  if partial:
    k1, k2 = partial[:2]
  else:
    k1, k2 = 'roi-begin', 'roi-end'

  stats_begin = {}
  stats = {}
  for line in (simstatsdelta or simstats):
    if line.startswith(k1+'.'):
      stats_begin[line.split()[0][len(k1+'.'):]] = long(line.split()[1])
    if line.startswith(k2+'.'):
      stats[line.split()[0][len(k2+'.'):]] = long(line.split()[1])

  if simstatsbase:
    # End stats may not be empty, check before adding the defaults
    if not stats:
      raise ValueError("Could not find stats in sim.stats (%s:%s)" % (k1, k2))
    for line in simstatsbase:
      line = line.strip()
      if not line: continue
      for c in range(ncores):
        key = line.split('[]')[0] + ('[%u]' % c) + line.split('[]', 1)[1]
        stats_begin.setdefault(key, 0)
        stats.setdefault(key, 0)
  else:
    if not stats or not stats_begin:
      raise ValueError("Could not find stats in sim.stats (%s:%s)" % (k1, k2))

  for key, value in stats.items():
    if key in stats_begin:
      value -= stats_begin[key]
      stats[key] = value
    if '[' in key:
      key = re.match('(.*)\[(.*)\](.*)', key).groups()
      key, core = key[0] + key[2], int(key[1])
    else:
      core = -1
    results.append((key, core, value))

  ## power.py
  power = {}
  if powerpy:
    exec(powerpy.read())
    for key, value in power.items():
      results.append(('power.%s' % key, -1, value))

  return results


def parse_results_from_dir(dirname, partial = None):
  files = []
  for filename in ('sim.stats', 'sim.stats.base', 'sim.stats.delta', 'sim.out', 'sim.cfg', 'stdout.txt', 'graphite.out', 'power.py'):
    fullname = os.path.join(dirname, filename)
    if os.path.exists(fullname):
      files.append(file(fullname))
    else:
      #sys.stderr.write('%s not available, some results may be missing\n' % filename)
      files.append(None)
  # if --partial was used with a filename, replace sim.stats with it
  if partial and len(partial) > 2:
    fullname = os.path.join(dirname, partial[2])
    if os.path.exists(fullname):
      files[0] = file(fullname)
    else:
      raise ValueError("Partial sim.stats replacement file named %s cannot be opened" % partial[2])
  return parse_results_from_fileobjs(files, partial = partial)


def get_results_file(filename, jobid = None, resultsdir = None, force = False):
  if jobid:
    return ic.job_output(jobid, filename, force)
  else:
    filename = os.path.join(resultsdir, filename)
    if os.path.exists(filename):
      return file(filename).read()
    else:
      return None


def format_size(size):
  i = 0
  while size > 1024:
    size /= 1024.
    i += 1
  return '%.1f%sB' % (size, [' ', 'K', 'M', 'G', 'T', 'P', 'E'][i])


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
