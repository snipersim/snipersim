import sys, os, sniper_lib

_, EVENT_MARKER, EVENT_THREAD_NAME, EVENT_APP_START, EVENT_APP_EXIT, EVENT_THREAD_CREATE, EVENT_THREAD_EXIT = range(7)

class SniperStatsBase:
  def parse_stats(self, (k1, k2), ncores, metrics = None):
    v1 = self.read_snapshot(k1, metrics = metrics)
    v2 = self.read_snapshot(k2, metrics = metrics)
    results = []
    for metricid in self.names.keys():
      name = '%s.%s' % self.names[metricid]
      if metrics and name not in metrics:
        continue
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

  def get_events(self):
    raise ValueError("Event information not available from statistics of this type")

  def get_markers(self):
    markers = {}
    for event, time, core, thread, arg0, arg1, s in self.get_events():
      if event == EVENT_MARKER:
        markers.append((time, core, thread, arg0, arg1, s))
    return markers

  def get_thread_names(self):
    names = {}
    for event, time, core, thread, arg0, arg1, s in self.get_events():
      if event == EVENT_THREAD_NAME:
        names[thread] = s
    return names

  def get_results(self, **kwds):
    return sniper_lib.get_results(stats = self, **kwds)


def SniperStats(resultsdir = '.', jobid = None):
  if jobid:
    import sniper_stats_jobid
    stats = sniper_stats_jobid.SniperStatsJobid(jobid)
  elif os.path.exists(os.path.join(resultsdir, 'sim.stats.sqlite3')):
    import sniper_stats_sqlite
    stats = sniper_stats_sqlite.SniperStatsSqlite(os.path.join(resultsdir, 'sim.stats.sqlite3'))
  elif os.path.exists(os.path.join(resultsdir, 'sim.stats.db')):
    import sniper_stats_db
    stats = sniper_stats_db.SniperStatsDb(os.path.join(resultsdir, 'sim.stats.db'))
  else:
    import sniper_stats_compat
    stats = sniper_stats_compat.SniperStatsCompat(resultsdir)
  stats.config = sniper_lib.get_config(jobid, resultsdir)
  return stats
