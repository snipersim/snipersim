"""
Make energy available as a statistic by running a partial McPAT on every statistics snapshot save

Works by registering a PRE_STAT_WRITE hook, which, before a stats snapshot write is triggered:
- Writes the current statistics to the database using the energystats-temp prefix
- Calls McPAT on the partial period (last-snapshot, energystats-temp)
- Processes the McPAT results, making them available through custom-callback statistics
- Finally the actual snapshot is written, including updated values for all energy counters
"""

import sys, os, sim


def build_dvfs_table(tech):
  # Build a table of (frequency, voltage) pairs.
  # Frequencies should be from high to low, and end with zero (or the lowest possible frequency)
  if tech == 22:
    return [ (2000, 1.0), (1800, 0.9), (1500, 0.8), (1000, 0.7), (0, 0.6) ]
  elif tech == 45:
    return [ (2000, 1.2), (1800, 1.1), (1500, 1.0), (1000, 0.9), (0, 0.8) ]
  else:
    raise ValueError('No DVFS table available for %d nm technology node' % tech)


class Power:
  def __init__(self, static, dynamic):
    self.s = static
    self.d = dynamic
  def __add__(self, v):
    return Power(self.s + v.s, self.d + v.d)
  def __sub__(self, v):
    return Power(self.s - v.s, self.d - v.d)


class EnergyStats:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    interval_ns = long(args.get(0, None) or 1000000) # Default power update every 1 ms
    sim.util.Every(interval_ns * sim.util.Time.NS, self.periodic, roi_only = True)
    self.dvfs_table = build_dvfs_table(int(sim.config.get('power/technology_node')))
    #
    self.name_last = None
    self.time_last_power = 0
    self.time_last_energy = 0
    self.in_stats_write = False
    self.power = {}
    self.energy = {}
    for metric in ('energy-static', 'energy-dynamic'):
      for core in range(sim.config.ncores):
        sim.stats.register('core', core, metric, self.get_stat)
        sim.stats.register('L1-I', core, metric, self.get_stat)
        sim.stats.register('L1-D', core, metric, self.get_stat)
        sim.stats.register('L2', core, metric, self.get_stat)
      #sim.stats.register_per_thread('core-'+metric, 'core', metric)
      #sim.stats.register_per_thread('L1-I-'+metric, 'L1-I', metric)
      #sim.stats.register_per_thread('L1-D-'+metric, 'L1-D', metric)
      #sim.stats.register_per_thread('L2-'+metric, 'L2', metric)
      sim.stats.register('processor', 0, metric, self.get_stat)
      sim.stats.register('dram', 0, metric, self.get_stat)

  def periodic(self, time, time_delta):
    self.update()

  def hook_pre_stat_write(self, prefix):
    if not self.in_stats_write:
      self.update()

  def hook_sim_end(self):
    if self.name_last:
      sim.util.db_delete(self.name_last, True)

  def update(self):
    if sim.stats.time() == self.time_last_power:
      # Time did not advance: don't recompute
      return
    if not self.power or (sim.stats.time() - self.time_last_power >= 10 * sim.util.Time.US):
      # Time advanced significantly, or no power result yet: compute power
      #   Save snapshot
      current = 'energystats-temp%s' % ('B' if self.name_last and self.name_last[-1] == 'A' else 'A')
      self.in_stats_write = True
      sim.stats.write(current)
      self.in_stats_write = False
      #   If we also have a previous snapshot: update power
      if self.name_last:
        power = self.run_power(self.name_last, current)
        self.update_power(power)
      #   Clean up previous last
      if self.name_last:
        sim.util.db_delete(self.name_last)
      #   Update new last
      self.name_last = current
      self.time_last_power = sim.stats.time()
    # Increment energy
    self.update_energy()

  def get_stat(self, objectName, index, metricName):
    if not self.in_stats_write:
      self.update()
    return self.energy.get((objectName, index, metricName), 0L)

  def update_power(self, power):
    def get_power(component, prefix = ''):
      return Power(component[prefix + 'Subthreshold Leakage'] + component[prefix + 'Gate Leakage'], component[prefix + 'Runtime Dynamic'])
    for core in range(sim.config.ncores):
      self.power[('L1-I', core)] = get_power(power['Core'][core], 'Instruction Fetch Unit/Instruction Cache/')
      self.power[('L1-D', core)] = get_power(power['Core'][core], 'Load Store Unit/Data Cache/')
      self.power[('L2',   core)] = get_power(power['Core'][core], 'L2/')
      self.power[('core', core)] = get_power(power['Core'][core]) - (self.power[('L1-I', core)] + self.power[('L1-D', core)] + self.power[('L2', core)])
    self.power[('processor', 0)] = get_power(power['Processor'])
    self.power[('dram', 0)] = get_power(power['DRAM'])

  def update_energy(self):
    if self.power and sim.stats.time() > self.time_last_energy:
      time_delta = sim.stats.time() - self.time_last_energy
      for (component, core), power in self.power.items():
        self.energy[(component, core, 'energy-static')] = self.energy.get((component, core, 'energy-static'), 0) + long(time_delta * power.s)
        self.energy[(component, core, 'energy-dynamic')] = self.energy.get((component, core, 'energy-dynamic'), 0) + long(time_delta * power.d)
      self.time_last_energy = sim.stats.time()

  def get_vdd_from_freq(self, f):
    # Assume self.dvfs_table is sorted from highest frequency to lowest
    for _f, _v in self.dvfs_table:
      if f >= _f:
        return _v
    assert ValueError('Could not find a Vdd for invalid frequency %f' % f)

  def gen_config(self, outputbase):
    freq = [ sim.dvfs.get_frequency(core) for core in range(sim.config.ncores) ]
    vdd = [ self.get_vdd_from_freq(f) for f in freq ]
    configfile = outputbase+'.cfg'
    cfg = open(configfile, 'w')
    cfg.write('''
[perf_model/core]
frequency[] = %s
[power]
vdd[] = %s
    ''' % (','.join(map(lambda f: '%f' % (f / 1000.), freq)), ','.join(map(str, vdd))))
    cfg.close()
    return configfile

  def run_power(self, name0, name1):
    outputbase = os.path.join(sim.config.output_dir, 'energystats-temp')

    configfile = self.gen_config(outputbase)

    os.system('unset PYTHONHOME; %s -d %s -o %s -c %s --partial=%s:%s --no-graph --no-text' % (
      os.path.join(os.getenv('SNIPER_ROOT'), 'tools/mcpat.py'),
      sim.config.output_dir,
      outputbase,
      configfile,
      name0, name1
    ))

    result = {}
    execfile(outputbase + '.py', {}, result)
    return result['power']

# All scripts execute in global scope, so other scripts will be able to call energystats.update()
energystats = EnergyStats()
sim.util.register(energystats)
