import collections, buildstack, colorsys

def build_itemlist(use_simple_sync = False, use_simple_mem = True):
  # List of all CPI contributors: <title>, <threshold (%)>, <contributors>
  # <contributors> can be string: key name in sim.stats (sans "roi-end.*[<corenum>].cpi")
  #                       list  : recursive list of sub-contributors
  #                       tuple : list of key names that are summed anonymously

  items = [
    [ 'base',           .01,   'Base' ],
    [ 'dispatch_width', .01,   'Issue' ],
    [ 'rs_full',        .01,   'RSFull' ],
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
    [ 'serial',   .01, ('Serialization', 'LongLatency') ], # FIXME: can LongLatency be anything other than MFENCE?
    [ 'smt',            .01,   'SMT' ],
    [ 'branch',   .01, 'BranchPredictor' ],
    [ 'itlb',     .01, 'ITLBMiss' ],
    [ 'dtlb',     .01, 'DTLBMiss' ],
    [ 'ifetch',   .01, (
          'DataCacheL1I', 'InstructionCacheL1I', 'InstructionCacheL1', 'InstructionCacheL1_S',
          'InstructionCacheL2', 'InstructionCacheL2_S', 'InstructionCacheL3', 'InstructionCacheL3_S',
          'InstructionCacheL4',  'InstructionCacheL4_S', 'InstructionCachemiss', 'InstructionCache????',
          'InstructionCachenuca-cache', 'InstructionCachedram-cache', 'InstructionCachedram',
          'InstructionCachedram-remote', 'InstructionCachecache-remote', 'InstructionCachedram-local',
          'InstructionCachepredicate-false', 'InstructionCacheunknown') ],
  ]
  if use_simple_mem:
    items += [
    [ 'mem',      .01, [
      [ 'l1d',      .01, ('DataCacheL1', 'DataCacheL1_S', 'PathLoadX', 'PathStore') ],
      [ 'l2',       .01, ('DataCacheL2', 'DataCacheL2_S') ],
      [ 'l3',       .01, ('DataCacheL3', 'DataCacheL3_S') ],
      [ 'l4',       .01, ('DataCacheL4', 'DataCacheL4_S') ],
      [ 'remote',   .01, 'DataCachecache-remote' ],
      [ 'nuca',     .01, 'DataCachenuca-cache' ],
      [ 'dram-cache', .01, 'DataCachedram-cache' ],
      [ 'dram',     .01, ('DataCachedram', 'DataCachedram-local', 'DataCachedram-remote',
                          'DataCachemiss', 'DataCache????', 'DataCachepredicate-false', 'DataCacheunknown') ],
    ] ],
  ]
  else:
    items += [
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
      [ 'nuca',         .01, 'DataCachenuca-cache' ],
      [ 'dram-cache', .01, 'DataCachedram-cache' ],
      [ 'dram',         .01, ('DataCachedram', 'DataCachedram-local', 'DataCachedram-remote',
                              'DataCachemiss', 'DataCache????', 'DataCachepredicate-false', 'DataCacheunknown') ],
    ] ],
  ]

  if use_simple_sync:
    items += [ [ 'sync', .01, ('SyncFutex', 'SyncPthreadMutex', 'SyncPthreadCond', 'SyncPthreadBarrier', 'SyncJoin',
                                   'SyncPause', 'SyncSleep', 'SyncUnscheduled', 'SyncMemAccess', 'Recv' ) ] ]
  else:
    items += [
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

  items += [
    [ 'dvfs-transition', 0.01, 'SyncDvfsTransition' ],
    [ 'imbalance', 0.01, [
      [ 'start', 0.01, 'StartTime' ],
      [ 'end',   0.01, 'Imbalance' ],
    ] ],
  ]

  return items


def build_grouplist(legacy = False):
  # List of <groupname>, <base color>, <list of items>
  # Used to collaps items when use_simple is true, and for coloring
  if legacy:
    return [
      ('compute',     (0xff,0,0), ('dispatch_width', 'rs_full', 'base', 'issue', 'depend',
                                   'branch', 'serial', 'smt')),
      ('communicate', (0,0xff,0), ('itlb','dtlb','ifetch','mem',)),
      ('synchronize', (0,0,0xff), ('sync', 'recv', 'dvfs-transition', 'imbalance')),
    ]
  else:
    return [
      ('compute',     (0xff,0,0),    ('dispatch_width', 'rs_full', 'base', 'issue', 'depend', 'serial', 'smt')),
      ('branch',      (0xff,0xff,0), ('branch',)),
      ('memory',      (0,0xff,0),    ('itlb','dtlb','ifetch','mem',)),
      ('synchronize', (0,0,0xff),    ('sync', 'recv', 'dvfs-transition', 'imbalance')),
    ]


class CpiItems:

  def __init__(self, items = None, groups = None, use_simple = False, use_simple_sync = False, use_simple_mem = True):
    self.items = items or build_itemlist(use_simple_sync, use_simple_mem)
    self.groups = groups or build_grouplist()
    if use_simple:
      self.compact_by_group()
    self.gen_contributions()
    self.names = buildstack.get_names(self.items)
    self.add_other()

  def compact_by_group(self):
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
    def findall(*keys): return tuple(_findall(self.items, keys))

    new_all_items = []
    new_simple_groups = []
    for name, color, items in self.groups:
      new_all_items.append([name, 0, findall(*items)])
      new_simple_groups.append((name, color, (name,)))
    self.items = new_all_items
    self.groups = new_simple_groups

  def gen_contributions(self):
    self.names_to_contributions = {}
    for group, color, members in self.groups:
      for name in buildstack.get_names(self.items, keys = members):
        self.names_to_contributions[name] = group

  def add_other(self):
    self.groups.append(('other', (0,0,0), ('other',)))
    self.names.append('other')
    self.names_to_contributions['other'] = 'other'

  def get_colors(self, labels_used = None):
    if labels_used:
      # Make sure labels is a sorted list of valid labels
      labels = [ label for label in self.names if label in labels_used ]
    else:
      labels = self.names
    return get_colors(labels, self.groups, self.names_to_contributions)


# Color helper functions

def color_tint_shade(base_color, num):
  base_color = map(lambda x:float(x)/255, base_color)
  base_color = colorsys.rgb_to_hsv(*base_color)
  colors = []
  delta = 0.6 / float((num/2) or 1)
  shade = 1.0
  for _ in range(num/2):
    shade -= delta
    colors.append((base_color[0],base_color[1],shade))
  colors = colors[::-1] # Reverse
  if num % 2 == 1:
    colors.append(base_color)
  tint = 1.0
  for _ in range(num/2):
    tint -= delta
    colors.append((base_color[0],tint,base_color[2]))
  colors = map(lambda x:colorsys.hsv_to_rgb(*x),colors)
  colors = map(lambda x:tuple(map(lambda y:int(y*255),x)),colors)
  return colors

def get_colors(labels, groups, names_to_contributions):
    contribution_counts = collections.defaultdict(int)
    for i in labels:
      contribution_counts[names_to_contributions[i]] += 1
    color_ranges = {}
    next_color_index = {}
    for name, color, _ in groups:
      color_ranges[name] = color_tint_shade(color, contribution_counts[name])
      next_color_index[name] = 0
    def get_next_color(contr):
      idx = next_color_index[contr]
      next_color_index[contr] += 1
      return color_ranges[contr][idx]
    return dict([ (label, get_next_color(names_to_contributions[label])) for label in labels ])
