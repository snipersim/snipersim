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
      vals1 = v1.get(metricid, {})
      vals2 = v2.get(metricid, {})
      results += [ (name, idx, vals2.get(idx, 0) - vals1.get(idx, 0)) for idx in range(id_min, id_max) ]
      if name == 'performance_model.elapsed_time' and idx < ncores:
        results += [ ('performance_model.elapsed_time_begin', idx, vals1.get(idx, 0)) for idx in range(ncores) ]
        results += [ ('performance_model.elapsed_time_end', idx, vals2.get(idx, 0)) for idx in range(ncores) ]
      elif name == 'barrier.global_time':
        results += [ ('barrier.global_time_begin', idx, vals1.get(idx, 0)) for idx in range(ncores) ]
        results += [ ('barrier.global_time_end', idx, vals2.get(idx, 0)) for idx in range(ncores) ]
    return results

  def get_topology(self):
    raise ValueError("Topology information not available from statistics of this type")

  def get_markers(self):
    raise ValueError("Marker information not available from statistics of this type")


def SniperStats(resultsdir = '.', jobid = None):
  if jobid:
    import sniper_stats_jobid
    return sniper_stats_jobid.SniperStatsJobid(jobid)
  if os.path.exists(os.path.join(resultsdir, 'sim.stats.sqlite3')):
    import sniper_stats_sqlite
    return sniper_stats_sqlite.SniperStatsSqlite(os.path.join(resultsdir, 'sim.stats.sqlite3'))
  elif os.path.exists(os.path.join(resultsdir, 'sim.stats.db')):
    import sniper_stats_db
    return sniper_stats_db.SniperStatsDb(os.path.join(resultsdir, 'sim.stats.db'))
  else:
    import sniper_stats_compat
    return sniper_stats_compat.SniperStatsCompat(resultsdir)
