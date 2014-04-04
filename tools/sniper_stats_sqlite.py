import collections, sqlite3, sniper_stats

class SniperStatsSqlite(sniper_stats.SniperStatsBase):
  def __init__(self, filename = 'sim.stats.sqlite3'):
    self.db = sqlite3.connect(filename)
    self.db.text_factory = str # Don't try to convert database contents to UTF-8
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

  def read_snapshot(self, prefix, metrics = None):
    c = self.db.cursor()
    c.execute('select prefixid from `prefixes` where prefixname = ?', (prefix,))
    prefixids = list(c)
    if prefixids:
      prefixid = prefixids[0][0]
      if metrics:
        nameids = [ str(nameid) for nameid, (objectname, metricname) in self.names.items() if '%s.%s' % (objectname, metricname) in metrics ]
        namefilter = ' and nameid in (%s)' % ','.join(nameids)
      else:
        namefilter = ''
      values = {}
      c = self.db.cursor()
      c.execute('select nameid, core, value from `values` where prefixid = ? %s' % namefilter, (prefixid,))
      for nameid, core, value in c:
        if nameid not in values: values[nameid] = {}
        values[nameid][core] = value
      return values
    else:
      raise ValueError('Invalid prefix %s' % prefix)

  def get_topology(self):
    c = self.db.cursor()
    return c.execute('SELECT componentname, coreid, masterid FROM topology').fetchall()

  def get_markers(self):
    c = self.db.cursor()
    if c.execute('SELECT name FROM sqlite_master WHERE type="table" AND name="marker"').fetchall():
      return c.execute('SELECT time, core, thread, value0, value1, description FROM marker').fetchall()
    else:
      return [ (timestamp, core, thread, value0, value1, description) for event, timestamp, core, thread, value0, value1, description in self.get_events() if event == sniper_stats.EVENT_MARKER ]

  def get_events(self):
    c = self.db.cursor()
    return c.execute('SELECT event, time, core, thread, value0, value1, description FROM event').fetchall()

if __name__ == '__main__':
  stats = SniperStatsSqlite()
  print stats.get_snapshots()
  print stats.read_snapshot('roi-end')
