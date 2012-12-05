import sys, os

class SniperStatsBase:
  def parse_stats(self, (k1, k2), ncores, metrics = None):
    v1 = self.read_snapshot(k1, metrics = metrics)
    v2 = self.read_snapshot(k2, metrics = metrics)
    results = []
    if metrics:
      keys_to_process = v2.keys()
    else:
      keys_to_process = self.names.keys()
    for metricid in keys_to_process:
      name = '%s.%s' % self.names[metricid]
      id_min = min(min(v2.get(metricid, {}).keys() or [0]), 0)
      id_max = max(max(v2.get(metricid, {}).keys() or [0])+1, ncores)
      for idx in range(id_min, id_max):
        val1 = v1.get(metricid, {}).get(idx, 0)
        val2 = v2.get(metricid, {}).get(idx, 0)
        results.append((name, idx, val2 - val1))
        if name == 'performance_model.elapsed_time' and idx < ncores:
          results.append(('performance_model.elapsed_time_begin', idx, val1))
          results.append(('performance_model.elapsed_time_end', idx, val2))
        elif name == 'barrier.global_time':
          results.append(('barrier.global_time_begin', idx, val1))
          results.append(('barrier.global_time_end', idx, val2))
    return results


def SniperStats(resultsdir):
  if os.path.exists(os.path.join(resultsdir, 'sim.stats.sqlite3')):
    import sniper_stats_sqlite
    return sniper_stats_sqlite.SniperStatsSqlite(os.path.join(resultsdir, 'sim.stats.sqlite3'))
  elif os.path.exists(os.path.join(resultsdir, 'sim.stats.db')):
    import sniper_stats_db
    return sniper_stats_db.SniperStatsDb(os.path.join(resultsdir, 'sim.stats.db'))
  else:
    import sniper_stats_compat
    return sniper_stats_compat.SniperStatsCompat(resultsdir)
