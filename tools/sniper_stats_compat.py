import os, re, sniper_stats

class SniperStatsCompat(sniper_stats.SniperStatsBase):
  def __init__(self, resultsdir):
    self.resultsdir = resultsdir

  def parse_stats(self, (k1, k2), ncores, metrics = None):
    simstats = os.path.join(self.resultsdir, 'sim.stats')
    simstats = os.path.exists(simstats) and open(simstats) or None

    simstatsbase = os.path.join(self.resultsdir, 'sim.stats.base')
    simstatsbase = os.path.exists(simstatsbase) and open(simstatsbase) or None

    simstatsdelta = os.path.join(self.resultsdir, 'sim.stats.delta')
    simstatsdelta = os.path.exists(simstatsdelta) and open(simstatsdelta) or None

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

    results = []

    for core in range(ncores):
      key = 'performance_model[%d].elapsed_time' % core
      if key in stats_begin:
        results.append(('performance_model.elapsed_time_begin', core, stats_begin[key]))
      if key in stats:
        results.append(('performance_model.elapsed_time_end', core, stats[key]))

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

    return results

  def get_snapshots(self):
    # Should be easily parseable from sim.stats.delta, but who cares
    raise NotImplementedError
