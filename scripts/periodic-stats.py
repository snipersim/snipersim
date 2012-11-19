"""
periodic-stats.py

Periodically write out all statistics
1st argument is the interval size in nanoseconds (default is 1e9 = 1 second of simulated time)
2nd argument, if "ins" then interval is number of instructions rather than nanoseconds
3rd argument, if present will limit the number of snapshots and dynamically remove itermediate data
"""

import sim

class PeriodicStats:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval = long(args.get(0, '') or 1e9)
    by_ins = args.get(1, '')
    self.max_snapshots = long(args.get(2, 0))
    self.num_snapshots = 0
    if by_ins == 'ins':
      self.interval = long(interval)
      sim.util.EveryIns(self.interval, self.periodic, roi_only = True)
    else:
      self.interval = long(interval * sim.util.Time.NS)
      sim.util.Every(self.interval, self.periodic, roi_only = True)

  def periodic(self, time, time_delta):
    if self.max_snapshots and self.num_snapshots > self.max_snapshots:
      self.num_snapshots /= 2
      cursor = sim.stats.db.cursor()
      for t in range(self.interval, time, self.interval * 2):
        prefixid = sim.stats.db.execute('SELECT prefixid FROM prefixes WHERE prefixname = ?', ('periodic-%d' % t,)).fetchall()
        if prefixid:
          cursor.execute('DELETE FROM prefixes WHERE prefixid = ?', (prefixid[0][0],))
          cursor.execute('DELETE FROM `values` WHERE prefixid = ?', (prefixid[0][0],))
      sim.stats.db.commit()
      self.interval *= 2

    if time % self.interval == 0:
      self.num_snapshots += 1
      sim.stats.write('periodic-%d' % time)

sim.util.register(PeriodicStats())
