"""
periodic-stats.py

Periodically write out all statistics
1st argument is the interval size in nanoseconds (default is 1e9 = 1 second of simulated time)
2rd argument, if present will limit the number of snapshots and dynamically remove itermediate data
"""

import sim

class PeriodicStats:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval = long(args.get(0, '') or 1000000000)
    self.max_snapshots = long(args.get(1, 0))
    self.num_snapshots = 0
    self.interval = long(interval * sim.util.Time.NS)
    self.next_interval = float('inf')
    self.in_roi = False
    self.have_deleted = False
    sim.util.Every(self.interval, self.periodic, roi_only = True)

  def hook_roi_begin(self):
    self.in_roi = True
    self.next_interval = sim.stats.time() + self.interval
    sim.stats.write('periodic-0')

  def hook_roi_end(self):
    self.next_interval = float('inf')
    self.in_roi = False

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
      self.have_deleted = True

    if time >= self.next_interval:
      self.num_snapshots += 1
      sim.stats.write('periodic-%d' % (self.num_snapshots * self.interval))
      self.next_interval += self.interval

  def hook_sim_end(self):
    if self.have_deleted:
      # We have deleted entries from the database, reclaim free space now
      sim.stats.db.cursor().execute('VACUUM')
      sim.stats.db.commit()

sim.util.register(PeriodicStats())
