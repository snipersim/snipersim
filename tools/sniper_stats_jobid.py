import sniper_stats, intelqueue, iqclient


class SniperStatsJobid(sniper_stats.SniperStatsBase):
  def __init__(self, jobid):
    self.jobid = jobid
    self.ic = iqclient.IntelClient()

  def get_snapshots(self):
    return self.ic.graphite_dbresults(self.jobid, 'get_snapshots')

  def read_snapshot(self, prefix, metrics = None):
    raise NotImplementedError('read_snapshot not currently supported, use sniper_lib.get_results instead')
    return self.ic.graphite_dbresults(self.jobid, 'read_snapshot', {'prefix': prefix, 'metrics': metrics})

  def get_topology(self):
    return self.ic.graphite_dbresults(self.jobid, 'get_topology')

  def get_markers(self):
    return self.ic.graphite_dbresults(self.jobid, 'get_markers')
