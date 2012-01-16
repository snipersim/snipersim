"""
tcp.py

Thread Criticality Predictor [bhattacharjee2009tcpfdpparmicm].
Keeps track of memory-related CPI component per core.
Defines a SimUtil command so the application can read TCP values per core.

From application, call SimUtil(SIM_USER_TCP, <coreid>) to get a core's absolute criticality metric (0..1000)
"""

import sim

SIM_USER_TCP = 0x0be00001
INTERVAL = 100000 # in ns
ncores = sim.config.ncores

CPI_MEM = (
  "cpiDataCacheL2", "cpiDataCacheL2_S", "cpiDataCacheL3", "cpiDataCacheL3_S",
  "cpiDataCachecache-remote", "cpiDataCachedram-local", "cpiDataCachedram-remote",
  "cpiDataCacheunknown",
)

class Tcp:
  def setup(self, args):
    self.sd = sim.util.StatsDelta()
    self.stats = {
      'time':   [ self.sd.getter("performance_model", core, "elapsed_time") for core in range(ncores) ],
      'instrs': [ self.sd.getter("performance_model", core, "instruction_count") for core in range(ncores) ],
      'cpimem': [ [ self.sd.getter("interval_timer", core, cpi) for cpi in CPI_MEM ] for core in range(ncores) ],
    }
    self.tcp = [ None for core in range(ncores) ]
    sim.util.Every(INTERVAL * sim.util.Time.NS, self.periodic, statsdelta = self.sd, roi_only = True)
    sim.util.register_command(SIM_USER_TCP, self.get_tcp)

  def periodic(self, time, time_delta):
    for core in range(ncores):
      cycles = self.stats['time'][core].delta * sim.dvfs.get_frequency(core) / 1e9 # convert fs to cycles
      instrs = self.stats['instrs'][core].delta
      cpimem = sum([ c.delta for c in self.stats['cpimem'][core] ])
      self.tcp[core] = int(1000 * cpimem / time_delta)

  def get_tcp(self, core_caller, arg):
    core = arg
    if 0 <= core < ncores:
      return self.tcp[core]
    else:
      return None


sim.util.register(Tcp())
