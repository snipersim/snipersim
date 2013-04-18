"""
dvfs.py

Change core frequencies according to a predifined list
Argument is a list of time,core,frequency values. Time is in nanoseconds, frequency in MHz
Example:
-sdvfs:1000:1:3000:2500:1:2660
Change core 1 to 3 GHz after 1 us, change core 1 to 2.66 GHz after 2.5 us
"""

import sys, os, sim

class Dvfs:
  def setup(self, args):
    self.events = []
    args = args.split(':')
    for i in range(0, len(args), 3):
      self.events.append((long(args[i])*sim.util.Time.NS, int(args[i+1]), int(args[i+2])))
    self.events.sort()
    sim.util.Every(100*sim.util.Time.NS, self.periodic, roi_only = True)

  def periodic(self, time, time_delta):
    while self.events and time >= self.events[0][0]:
      t, cpu, freq = self.events[0]
      self.events = self.events[1:]
      sim.dvfs.set_frequency(cpu, freq)

sim.util.register(Dvfs())
