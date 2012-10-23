import sys, os

def SniperStats(resultsdir):
  if os.path.exists(os.path.join(resultsdir, 'sim.stats.db')):
    import sniper_stats_db
    return sniper_stats_db.SniperStatsDb(os.path.join(resultsdir, 'sim.stats.db'))
  else:
    import sniper_stats_compat
    return sniper_stats_compat.SniperStatsCompat(resultsdir)
