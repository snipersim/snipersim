"""
Make energy available as a statistic by running a partial McPAT on every statistics snapshot save

Works by registering a PRE_STAT_WRITE hook, which, before a stats snapshot write is triggered:
- Writes the current statistics to the database using the energystats-temp prefix
- Calls McPAT on the partial period (last-snapshot, energystats-temp)
- Processes the McPAT results, making them available through custom-callback statistics
- Finally the actual snapshot is written, including updated values for all energy counters
"""

import sys, os, sim

class EnergyStats:
  def setup(self, args):
    self.name_last = None
    self.time_last = 0
    self.in_stats_write = False
    self.power = {}
    self.energy = {}
    for core in range(sim.config.ncores):
      sim.stats.register('core', core, 'energy.static', self.get_stat)
      sim.stats.register('core', core, 'energy.dynamic', self.get_stat)
    sim.stats.register('processor', 0, 'energy.static', self.get_stat)
    sim.stats.register('processor', 0, 'energy.dynamic', self.get_stat)
    sim.stats.register('dram', 0, 'energy.static', self.get_stat)
    sim.stats.register('dram', 0, 'energy.dynamic', self.get_stat)

  def hook_pre_stat_write(self, prefix):
    if not self.in_stats_write:
      if self.name_last and sim.stats.time() > self.time_last:
        if not self.power or (sim.stats.time() - self.time_last >= 1 * sim.util.Time.US):
          self.in_stats_write = True
          sim.stats.write('energystats-temp')
          self.in_stats_write = False
          power = self.run_power(self.name_last, 'energystats-temp')
          sim.util.db_delete('energystats-temp')
          self.update_power(power)
        self.update_energy()
      self.name_last = prefix

  def get_stat(self, objectName, index, metricName):
    return self.energy.get((objectName, index, metricName), 0L)

  def update_power(self, power):
    def get_power_static(component):
      return component['Subthreshold Leakage'] + component['Gate Leakage']
    def get_power_dynamic(component):
      return component['Peak Dynamic']
    for core in range(sim.config.ncores):
      self.power[('core', core, 'energy.static')] = get_power_static(power['Core'][core])
      self.power[('core', core, 'energy.dynamic')] = get_power_dynamic(power['Core'][core])
    self.power[('processor', 0, 'energy.static')] = get_power_static(power['Processor'])
    self.power[('processor', 0, 'energy.dynamic')] = get_power_dynamic(power['Processor'])
    self.power[('dram', 0, 'energy.static')] = get_power_static(power['DRAM'])
    self.power[('dram', 0, 'energy.dynamic')] = get_power_dynamic(power['DRAM'])

  def update_energy(self):
    time_delta = sim.stats.time() - self.time_last
    for key, power in self.power.items():
      self.energy[key] = self.energy.get(key, 0) + long(time_delta * power)
    self.time_last = sim.stats.time()

  def run_power(self, name0, name1):
    outputbase = os.path.join(sim.config.output_dir, 'energystats-temp')
    os.system('unset PYTHONHOME; %s -d %s -o %s --partial=%s:%s --no-graph --no-text' % (
      os.path.join(os.getenv('GRAPHITE_ROOT'), 'tools/mcpat.py'),
      sim.config.output_dir,
      outputbase,
      name0, name1
    ))
    result = {}
    execfile(outputbase + '.py', {}, result)
    return result['power']

sim.util.register(EnergyStats())
