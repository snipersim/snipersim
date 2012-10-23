import collections, sqlite3, sniper_stats

class SniperStatsSqlite(sniper_stats.SniperStatsBase):
  def __init__(self, filename = 'sim.stats.sqlite3'):
    self.db = sqlite3.connect(filename)
    self.names = self.read_metricnames()

  def get_snapshots(self):
    snapshots = []
    c = self.db.cursor()
    c.execute('select prefixid, prefixname from `prefixes` order by prefixid asc')
    for prefixid, prefixname in c:
      snapshots.append(prefixname)
    return snapshots

  def read_metricnames(self):
    names = {}
    c = self.db.cursor()
    c.execute('select nameid, objectname, metricname from `names`')
    for nameid, objectname, metricname in c:
      names[nameid] = (objectname, metricname)
    return names

  def read_snapshot(self, prefix):
    c = self.db.cursor()
    c.execute('select prefixid from `prefixes` where prefixname = ?', (prefix,))
    prefixids = list(c)
    if prefixids:
      prefixid = prefixids[0][0]
      values = collections.defaultdict(dict)
      c = self.db.cursor()
      c.execute('select nameid, core, value from `values` where prefixid = ?', (prefixid,))
      for nameid, core, value in c:
        values[nameid][core] =  value
      return values
    else:
      raise ValueError('Invalid prefix %s' % prefix)


if __name__ == '__main__':
  stats = SniperStatsSqlite()
  print stats.get_snapshots()
  print stats.read_snapshot('roi-end')
