import collections, sniper_lib, sniper_config

class CpiData:

  def __init__(self, jobid = '', resultsdir = '', config = None, stats = None, data = None, partial = None):
    if data:
      data_raw = data
    else:
      data_raw = sniper_lib.get_results(jobid = jobid, resultsdir = resultsdir, config = config, stats = stats, partial = partial)
    self.stats = data_raw['results']
    self.config = data_raw['config']
    self.parse()

  def parse(self):
    ncores = int(self.config['general/total_cores'])
    instrs = self.stats['performance_model.instruction_count']
    try:
      times = self.stats['performance_model.elapsed_time']
      cycles_scale = self.stats['fs_to_cycles_cores']
    except KeyError:
      # On error, assume that we are using the pre-DVFS version
      times = self.stats['performance_model.cycle_count']
      cycles_scale = [ 1. for idx in range(ncores) ]
    time0_begin = self.stats['global.time_begin']
    time0_end = self.stats['global.time_end']
    times = [ self.stats['performance_model.elapsed_time_end'][core] - time0_begin for core in range(ncores) ]

    if self.stats.get('fastforward_performance_model.fastforwarded_time', [0])[0]:
      fastforward_scale = times[0] / (times[0] - self.stats['fastforward_performance_model.fastforwarded_time'][0])
      times = [ t-f for t, f in zip(times, self.stats['fastforward_performance_model.fastforwarded_time']) ]
    else:
      fastforward_scale = 1.
    if 'performance_model.cpiFastforwardTime' in self.stats:
      del self.stats['performance_model.cpiFastforwardTime']


    data = collections.defaultdict(collections.defaultdict)
    for key, values in self.stats.items():
      if '.cpi' in key:
        if key.startswith('thread.'):
          # Ignore per-thread statistics
          continue
        key = key.split('.cpi')[1]
        for core in range(ncores):
          data[core][key] = values[core] * cycles_scale[core]

    if not data:
      raise ValueError('No .cpi data found, simulation did not use the interval core model')

    # Split up cpiBase into 1/issue and path dependencies
    for core in range(ncores):
      if data[core].get('SyncMemAccess', 0) == data[core].get('SyncPthreadBarrier', 0):
        # Work around a bug in iGraphite where SyncMemAccess wrongly copied from SyncPthreadBarrier
        # Since SyncMemAccess usually isn't very big anyway, setting it to zero should be accurate enough
        # For simulations with a fixed version of iGraphite, the changes of SyncMemAccess being identical to
        #   SyncPthreadBarrier, down to the last femtosecond, are slim, so this code shouldn't trigger
        data[core]['SyncMemAccess'] = 0
      if data[core].get('StartTime') == None and 'performance_model.idle_elapsed_time' in self.stats:
        # Fix a bug whereby the start time was not being reported in the CPI stacks correctly
        data[core]['StartTime'] = cycles_scale * self.stats['performance_model.idle_elapsed_time'][core] - \
                                  data[core]['SyncFutex']       - data[core]['SyncPthreadMutex']    - \
                                  data[core]['SyncPthreadCond'] - data[core]['SyncPthreadBarrier']  - \
                                  data[core]['Recv']
      # Critical path accounting
      cpContrMap = {
        # critical path components
        'interval_timer.cpContr_generic': 'PathInt',
        'interval_timer.cpContr_store': 'PathStore',
        'interval_timer.cpContr_load_other': 'PathLoadX',
        'interval_timer.cpContr_branch': 'PathBranch',
        'interval_timer.cpContr_load_l1': 'DataCacheL1',
        'interval_timer.cpContr_load_l2': 'DataCacheL2',
        'interval_timer.cpContr_load_l3': 'DataCacheL3',
        'interval_timer.cpContr_fp_addsub': 'PathFP',
        'interval_timer.cpContr_fp_muldiv': 'PathFP',
        # issue ports
        'interval_timer.cpContr_port0': 'PathP0',
        'interval_timer.cpContr_port1': 'PathP1',
        'interval_timer.cpContr_port2': 'PathP2',
        'interval_timer.cpContr_port34': 'PathP34',
        'interval_timer.cpContr_port5': 'PathP5',
        'interval_timer.cpContr_port05': 'PathP05',
        'interval_timer.cpContr_port015': 'PathP015',
      }
      for k in self.stats:
        if k.startswith('interval_timer.cpContr_'):
          if k not in cpContrMap.keys():
            print 'Missing in cpContrMap: ', k
      # Keep 1/width as base CPI component, break down the remainder according to critical path contributors
      BaseBest = instrs[core] / float(sniper_config.get_config(self.config, 'perf_model/core/interval_timer/dispatch_width', core))
      BaseAct = data[core]['Base']
      BaseCp = BaseAct - BaseBest
      scale = BaseCp / (BaseAct or 1)
      for cpName, cpiName in cpContrMap.items():
        val = float(self.stats.get(cpName, [0]*ncores)[core]) / 1e6
        data[core]['Base'] -= val * scale
        data[core][cpiName] = data[core].get(cpiName, 0) + val * scale
      # Issue width
      for key, values in self.stats.items():
        if key.startswith('interval_timer.detailed-cpiBase-'):
          if 'DispatchWidth' in key:
            if 'DispatchRate' not in key: # We already accounted for DispatchRate above, don't do it twice
              data[core]['Base'] -= values[core]
              data[core]['Issue'] = data[core].get('Issue', 0) + values[core]
      # Fix up large cpiSync fractions that started before but ended inside our interval
      time0_me = 'performance_model.elapsed_time_begin' in self.stats and self.stats['performance_model.elapsed_time_begin'][core] or 0
      if time0_me < time0_begin:
        time0_extra = time0_begin - time0_me
        #    Number of cycles that weren't accounted for when starting this interval
        cycles_extra = time0_extra * cycles_scale[core]
        #    Components that could be the cause of cycles_extra. It should be just one, but if there's many, we'll have to guess
        sync_components = dict([ (key, value) for key, value in data[core].items() if (key.startswith('Sync') or key == 'StartTime') and value > cycles_extra ])
        sync_total = sum(sync_components.values())
        for key, value in sync_components.items():
          data[core][key] -= cycles_extra*value/float(sync_total)
      data[core]['Imbalance'] = cycles_scale[core] * max(times) - sum(data[core].values())

    self.data = data
    self.ncores = ncores
    self.cores = range(ncores)
    self.instrs = instrs
    self.times = times
    self.cycles_scale = cycles_scale
    self.fastforward_scale = fastforward_scale


  def get_compfrac(self):
    max_time = self.cycles_scale[0] * max(self.times)
    return dict([ (
      core,
      1 - (self.data[core].get('StartTime', 0) + self.data[core].get('Imbalance', 0) + self.data[core].get('SyncPthreadCond', 0) + \
           self.data[core].get('SyncPthreadBarrier', 0) + self.data[core].get('SyncJoin', 0) + self.data[core].get('Recv', 0)) / (float(max_time) or 1.)
    ) for core in self.data.keys() ])


  def filter(self, cores_list = None, core_mincomp = 0):
    if not cores_list:
      cores_list = self.cores

    if core_mincomp:
      compfrac = self.get_compfrac()
      cores_list = [ core for core in cores_list if compfrac[core] >= core_mincomp ]

    self.data = dict([ (core, self.data[core]) for core in cores_list ])
    self.instrs = dict([ (core, self.instrs[core]) for core in cores_list ])
    self.ncores = len(cores_list)
    self.cores = cores_list


  def aggregate(self):
    allkeys = self.data[self.cores[0]].keys()
    self.data = { 0: dict([ (key, sum([ self.data[core][key] for core in self.cores ]) / len(self.cores)) for key in allkeys ]) }
    self.instrs = { 0: sum(self.instrs[core] for core in self.cores) / len(self.cores) }
    self.ncores = 1
    self.cores = [0]
