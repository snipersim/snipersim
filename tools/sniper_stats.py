import sys, os

class SniperStatsBase:
  def parse_stats(self, (k1, k2), ncores):
    v1 = self.read_snapshot(k1)
    v2 = self.read_snapshot(k2)
    results = []
    for metricid in self.names.keys():
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
