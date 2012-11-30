import buildstack

class CpiItems:
  def __init__(self, use_simple = False, use_simple_sync = False, use_simple_mem = True):
    # List of all CPI contributors: <title>, <threshold (%)>, <contributors>
    # <contributors> can be string: key name in sim.stats (sans "roi-end.*[<corenum>].cpi")
    #                       list  : recursive list of sub-contributors
    #                       tuple : list of key names that are summed anonymously

    all_items = [
      [ 'dispatch_width', .01,   'Issue' ],
      [ 'base',           .01,   'Base' ],
      [ 'depend',   .01,   [
        [ 'int',      .01, 'PathInt' ],
        [ 'fp',       .01, 'PathFP' ],
        [ 'branch',   .01, 'PathBranch' ],
      ] ],
      [ 'issue',    .01, [
        [ 'port0',        .01,    'PathP0' ],
        [ 'port1',        .01,    'PathP1' ],
        [ 'port2',       .01,    'PathP2' ],
        [ 'port34',        .01,    'PathP34' ],
        [ 'port5',        .01,    'PathP5' ],
        [ 'port05',      .01,    'PathP05' ],
        [ 'port015',      .01,    'PathP015' ],
      ] ],
      [ 'branch',   .01, 'BranchPredictor' ],
      [ 'serial',   .01, ('Serialization', 'LongLatency') ], # FIXME: can LongLatency be anything other than MFENCE?
      [ 'smt',            .01,   'SMT' ],
      [ 'itlb',     .01, 'ITLBMiss' ],
      [ 'dtlb',     .01, 'DTLBMiss' ],
      [ 'ifetch',   .01, (
            'DataCacheL1I', 'InstructionCacheL1I', 'InstructionCacheL1', 'InstructionCacheL1_S',
            'InstructionCacheL2', 'InstructionCacheL2_S', 'InstructionCacheL3', 'InstructionCacheL3_S',
            'InstructionCacheL4',  'InstructionCacheL4_S', 'InstructionCachemiss', 'InstructionCache????',
            'InstructionCachedram-cache', 'InstructionCachedram',
            'InstructionCachedram-remote', 'InstructionCachecache-remote', 'InstructionCachedram-local',
            'InstructionCachepredicate-false', 'InstructionCacheunknown') ],
    ]
    if use_simple_mem:
      all_items += [
      [ 'mem',      .01, [
        [ 'l1d',      .01, ('DataCacheL1', 'DataCacheL1_S', 'PathLoadX', 'PathStore') ],
        [ 'l2',       .01, ('DataCacheL2', 'DataCacheL2_S') ],
        [ 'l3',       .01, ('DataCacheL3', 'DataCacheL3_S') ],
        [ 'l4',       .01, ('DataCacheL4', 'DataCacheL4_S') ],
        [ 'remote',   .01, 'DataCachecache-remote' ],
        [ 'dram-cache', .01, 'DataCachedram-cache' ],
        [ 'dram',     .01, ('DataCachedram', 'DataCachedram-local', 'DataCachedram-remote', 'DataCachemiss', 'DataCache????', 'DataCachepredicate-false', 'DataCacheunknown') ],
      ] ],
    ]
    else:
      all_items += [
      [ 'mem',      .01, [
        [ 'l0d_neighbor', .01, 'DataCacheL1_S' ],
        [ 'l1d',          .01, ('DataCacheL1', 'PathLoadX', 'PathStore') ],
        [ 'l1_neighbor',  .01, 'DataCacheL2_S' ],
        [ 'l2',           .01, 'DataCacheL2' ],
        [ 'l2_neighbor',  .01, 'DataCacheL3_S' ],
        [ 'l3',           .01, 'DataCacheL3' ],
        [ 'l3_neighbor',  .01, 'DataCacheL4_S' ],
        [ 'l4',           .01, 'DataCacheL4' ],
        [ 'off_socket',   .01, 'DataCachecache-remote' ],
        [ 'dram',         .01, ('DataCachedram-local', 'DataCachedram-remote', 'DataCachemiss', 'DataCache????', 'DataCachepredicate-false', 'DataCacheunknown') ],
      ] ],
    ]

    if use_simple_sync:
      all_items += [ [ 'sync', .01, ('SyncFutex', 'SyncPthreadMutex', 'SyncPthreadCond', 'SyncPthreadBarrier', 'SyncJoin',
                                     'SyncPause', 'SyncSleep', 'SyncUnscheduled', 'SyncMemAccess', 'Recv' ) ] ]
    else:
      all_items += [
      [ 'sync',     .01, [
        [ 'futex',    .01, 'SyncFutex' ],
        [ 'mutex',    .01, 'SyncPthreadMutex' ],
        [ 'cond',     .01, 'SyncPthreadCond' ],
        [ 'barrier',  .01, 'SyncPthreadBarrier' ],
        [ 'join',     .01, 'SyncJoin' ],
        [ 'pause',    .01, 'SyncPause' ],
        [ 'sleep',    .01, 'SyncSleep' ],
        [ 'unscheduled', .01, 'SyncUnscheduled' ],
        [ 'memaccess',.01, 'SyncMemAccess' ],
        [ 'recv',     .01, 'Recv' ],
      ] ],
    ]

    all_items += [
      [ 'dvfs-transition', 0.01, 'SyncDvfsTransition' ],
      [ 'imbalance', 0.01, [
        [ 'start', 0.01, 'StartTime' ],
        [ 'end',   0.01, 'Imbalance' ],
      ] ],
    ]

    def _findall(items, keys = None):
      res = []
      for name, threshold, key_or_items in items:
        if not keys or name in keys:
          if type(key_or_items) is list:
            res += _findall(key_or_items)
          elif type(key_or_items) is tuple:
            res += list(key_or_items)
          else:
            res += [ key_or_items ]
      return res
    def findall(*keys): return tuple(_findall(all_items, keys))

    simple_groups = (
      ('compute', ('dispatch_width', 'base', 'issue', 'depend',
                  'branch', 'serial', 'smt')),
      ('communicate', ('itlb','dtlb','ifetch','mem',)),
      ('synchronize', ('sync', 'recv', 'dvfs-transition', 'imbalance')),
    )

    if use_simple:
      new_all_items = []
      new_simple_groups = []
      for k,v in simple_groups:
        new_all_items.append([k, 0, findall(*v)])
        new_simple_groups.append((k,(k,)))
      all_items = new_all_items
      simple_groups = new_simple_groups

    all_names = buildstack.get_names(all_items)

    base_contribution = {}
    for group, members in simple_groups:
      for name in buildstack.get_names(all_items, keys = members):
        base_contribution[name] = group

    self.items = all_items
    self.names = all_names
    self.names_to_contributions = base_contribution
