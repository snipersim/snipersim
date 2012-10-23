import sys, os

class SniperStatsBase:
  def parse_stats(self, (k1, k2), ncores):
    v1 = self.read_snapshot(k1)
    v2 = self.read_snapshot(k2)
    results = []
    for metricid, list in v2.items():
      name = '%s.%s' % self.names[metricid]
      for idx in range(min(0, min(list.keys() or [0])), max(ncores, max(list.keys() or [0])+1)):
        val1 = v1.get(metricid, {}).get(idx, 0)
        val2 = v2.get(metricid, {}).get(idx, 0)
        results.append((name, idx, val2 - val1))
        if name == 'performance_model.elapsed_time' and idx < ncores:
          results.append(('performance_model.elapsed_time_begin', idx, val1))
          results.append(('performance_model.elapsed_time_end', idx, val2))
    return results


def SniperStats(resultsdir):
  if os.path.exists(os.path.join(resultsdir, 'sim.stats.db')):
    import sniper_stats_db
    return sniper_stats_db.SniperStatsDb(os.path.join(resultsdir, 'sim.stats.db'))
  else:
    import sniper_stats_compat
    return sniper_stats_compat.SniperStatsCompat(resultsdir)
