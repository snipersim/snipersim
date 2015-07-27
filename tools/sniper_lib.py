# A copy of this file is distributed with the binaries of Sniper and Benchmarks

import sys, os, re, subprocess, cStringIO, sniper_stats, sniper_config
try:
  import json
except ImportError:
  import localjson as json


try:
  try:
    import env_setup
    sys.path.append(os.path.join(env_setup.benchmarks_root(), 'tools', 'scheduler'))
  except EnvironmentError, e:
    pass
  import intelqueue, iqclient, packdir, app_constraints
  ic = iqclient.IntelClient()
  ic_invalid = False
except ImportError:
  ic_invalid = True


class SniperResultsException(Exception): pass


def get_config(jobid = None, resultsdir = None, force_deleted = True):
  if jobid:
    if ic_invalid:
      raise RuntimeError('Cannot fetch results from server, make sure BENCHMARKS_ROOT points to a valid copy of benchmarks+iqlib')
    simcfg = ic.job_output(jobid, 'sim.cfg', force_deleted)
  elif resultsdir:
    cfgfile = os.path.join(resultsdir, 'sim.cfg')
    if not os.path.exists(cfgfile):
      raise ValueError('Cannot find config file at %s' % resultsdir)
    simcfg = file(cfgfile).read()
  config = sniper_config.parse_config(simcfg)
  return config


def get_results(jobid = None, resultsdir = None, config = None, stats = None, partial = None, force = False, metrics = None):
  if jobid:
    if ic_invalid:
      raise RuntimeError('Cannot fetch results from server, make sure BENCHMARKS_ROOT points to a valid copy of benchmarks+iqlib')
    results = ic.graphite_results(jobid, partial, metrics)
    config = get_config(jobid = jobid, force_deleted = force)
  elif resultsdir:
    results = parse_results_from_dir(resultsdir, partial = partial, metrics = metrics)
    config = get_config(resultsdir = resultsdir)
  elif stats:
    config = config or stats.config
    results = stats.parse_stats(partial or ('roi-begin', 'roi-end'), int(config['general/total_cores']), metrics = metrics)
  else:
    raise ValueError('Need either jobid or resultsdir')

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
  # Figure out when the interval of time, represented by partial, actually begins/ends
  # Since cores can account for time in chunks, per-core time can be
  # both before (``wakeup at future time X'') or after (``sleep until woken up'')
  # the current time.
  if 'barrier.global_time_begin' in stats:
    # Most accurate: ask the barrier
    time0_begin = stats['barrier.global_time_begin'][0]
    time0_end = stats['barrier.global_time_end'][0]
    stats.update({'global.time_begin': time0_begin, 'global.time_end': time0_end, 'global.time': time0_end - time0_begin})
  elif 'performance_model.elapsed_time_begin' in stats:
    # Guess based on core that has the latest time (future wakeup is less common than sleep on futex)
    time0_begin = max(stats['performance_model.elapsed_time_begin'])
    time0_end = max(stats['performance_model.elapsed_time_end'])
    stats.update({'global.time_begin': time0_begin, 'global.time_end': time0_end, 'global.time': time0_end - time0_begin})
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
  # Fixed versions of [idle|nonidle] elapsed time
  if 'performance_model.elapsed_time' in stats and 'performance_model.idle_elapsed_time' in stats:
    stats['performance_model.nonidle_elapsed_time'] = [
      stats['performance_model.elapsed_time'][c] - stats['performance_model.idle_elapsed_time'][c]
      for c in range(ncores)
    ]
    stats['performance_model.idle_elapsed_time'] = [
      time0_end - time0_begin - stats['performance_model.nonidle_elapsed_time'][c]
      for c in range(ncores)
    ]
    stats['performance_model.elapsed_time'] = [ time0_end - time0_begin for c in range(ncores) ]
  # DVFS-enabled runs: emulate cycle_count asuming constant (initial) frequency
  if 'performance_model.elapsed_time' in stats and 'performance_model.cycle_count' not in stats:
    stats['performance_model.cycle_count'] = [ stats['fs_to_cycles_cores'][idx] * stats['performance_model.elapsed_time'][idx] for idx in range(ncores) ]
  if 'thread.nonidle_elapsed_time' in stats and 'thread.nonidle_cycle_count' not in stats:
    stats['thread.nonidle_cycle_count'] = [ long(stats['fs_to_cycles'] * t) for t in stats['thread.nonidle_elapsed_time'] ]
  # IPC
  if 'performance_model.cycle_count' in stats:
    stats['ipc'] = [
      i / (c or 1)
      for i, c in zip(stats['performance_model.instruction_count'], stats['performance_model.cycle_count'])
    ]

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

  if not partial:
    walltime = [ v for k, _, v in results if k == 'time.walltime' ]
    instrs = [ v for k, _, v in results if k == 'core.instructions' ]
    if walltime and instrs:
      walltime = walltime[0] / 1e6 # microseconds -> seconds
      instrs = sum(instrs)
      results.append(('roi.walltime', -1, walltime))
      results.append(('roi.instrs', -1, instrs))
      results.append(('roi.ipstotal', -1, instrs / walltime))
      results.append(('roi.ipscore', -1, instrs / (walltime * ncores)))

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


def sign(x):
  if x > 0:
    return 1
  elif x < 0:
    return -1
  else:
    return 0


def scale_sci(value):
  i = 0
  s = sign(value)
  value = abs(value)
  if value < 1 and value > 1e-15:
    while value <= .01:
      value *= 1000.
      i += 1
    return (s * value, ' munpf'[i])
  else:
    while value >= 1000:
      value /= 1000.
      i += 1
    return (s * value, ' kMGTPE'[i])


#
# The first few fields of the stat line should be:
#
#  pid   %d (1) The process ID
#  comm  %s (2) Filename of executable, in parentheses
#  state %c (3) State
#  ppid  %d (4) ppid --- the value we want to return.
def parse_stat_line_for_ppid(stat):
  # Split string into two halves: the part before the
  # comm, which should end with a ')', and the part after.
  stat_string = stat.rsplit(')',1)

  if (len(stat_string) >= 1):
     # Now split every after comm into tokens by white space.
     stat_tokens = stat_string[1].split()
     if (len(stat_tokens) >= 2):
       ppid_string = stat_tokens[1]
       try:
          return int(ppid_string)
       except ValueError:
          pass
  raise ValueError("Unable to parse stat line %s" % stat)

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
    ppid = parse_stat_line_for_ppid(stat)
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


class OutputToLess:
  def __enter__(self):
    if sys.stdout.isatty():
      self.stream = cStringIO.StringIO()
      sys.stdout = self.stream
    else:
      self.stream = None
  def __exit__(self, exc_type, exc_value, traceback):
    if self.stream:
      sys.stdout.flush()
      sys.stdout = sys.__stdout__
      if exc_type:
        # Dump output up to error
        print self.stream.getvalue(),
        # Process exception normally
        return False
      data = self.stream.getvalue()
      if len(data) > 0:
        less = subprocess.Popen([ 'less', '--no-init', '--quit-if-one-screen', '--chop-long-lines' ], stdin = subprocess.PIPE)
        try:
          less.stdin.write(data)
          less.stdin.close()
        except IOError:
          # Ignore broken pipe when less doesn't read all of its input
          pass
        while True:
          try:
            less.wait()
            break
          except KeyboardInterrupt:
            # Ignore Ctrl+C to avoid aborting less before it restored the terminal settings
            pass
    elif exc_type:
      # Process exception normally
      return False
