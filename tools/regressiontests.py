#!/usr/bin/env python

import sys, os
sys.path.append(os.path.join(os.getenv('BENCHMARKS_ROOT'), 'tools', 'scheduler'))
import intelqueue, iqclient, iqlib
ic = iqclient.IntelClient()

JOBGROUP = 'regress'

benchmarks_splash = ('splash2-barnes', 'splash2-cholesky', 'splash2-fft', 'splash2-fmm', 'splash2-lu.cont', 'splash2-lu.ncont', 'splash2-ocean.cont', 'splash2-ocean.ncont', 'splash2-radiosity', 'splash2-radix', 'splash2-raytrace', 'splash2-volrend', 'splash2-water.nsq', 'splash2-water.sp')
benchmarks_parsec = ('parsec-blackscholes', 'parsec-canneal', 'parsec-dedup', 'parsec-streamcluster', 'parsec-swaptions', 'parsec-ferret', 'parsec-freqmine', 'parsec-facesim', 'parsec-fluidanimate', 'parsec-raytrace', 'parsec-bodytrack')
# 'parsec-vips': too many threads (Redmine #111)
# 'parsec-x264': not enough parallelism (Redmine #110)
benchmarks_rodinia = ('rodinia-backprop', 'rodinia-bfs', 'rodinia-cfd', 'rodinia-hotspot', 'rodinia-heartwall', 'rodinia-leukocyte', 'rodinia-lud', 'rodinia-needlemanwunsch', 'rodinia-kmeans', 'rodinia-srad')
benchmarks_ompm2001 = ('ompm2001-wupwise_m', 'ompm2001-swim_m', 'ompm2001-mgrid_m', 'ompm2001-applu_m', 'ompm2001-equake_m', 'ompm2001-apsi_m', 'ompm2001-fma3d_m', 'ompm2001-art_m', 'ompm2001-ammp_m', 'ompm2001-galgel_m', 'ompm2001-gafort_m')
benchmarks_corevalidation = ('core_validation-bsearch', 'core_validation-cache_dram', 'core_validation-cache_l1', 'core_validation-cache_l2', 'core_validation-cache_l3', 'core_validation-dijkstra', 'core_validation-dl1', 'core_validation-fp_mul', 'core_validation-fp_mul2', 'core_validation-memory', 'core_validation-mul', 'core_validation-pcache_dram', 'core_validation-pcache_l1', 'core_validation-pcache_l2', 'core_validation-pcache_l3', 'core_validation-qsort', 'core_validation-dvfs_sync_independent', 'core_validation-dvfs_sync_barrier', 'core_validation-dvfs_sync_dram', 'core_validation-dvfs_sync_steal')

class Test:
  def __init__(self, name, program, inputsize, nthreads, config = [], graphiteoptions = []):
    self.name = name
    self.program = program
    self.inputsize = inputsize
    self.nthreads = nthreads
    self.config = config
    self.graphiteoptions = graphiteoptions
  def submit(self, gitid):
    return iqlib.graphite_submit(
      ic, JOBGROUP, self.name, 5, gitid, 'gitid',
      self.program, self.inputsize, self.nthreads,
      ' '.join('-c %s' % cnf for cnf in self.config) + ' ' + \
      ' '.join('-g %s' % opt for opt in self.graphiteoptions) + ' ' + \
      '--power',
      files = [], constraints = {})

# Test definitions

TESTS = {
  'daily': {
    'master': [
      Test('dunnington-interval', 'splash2-fft', 'small',  1, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'splash2-fft', 'small',  4, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'splash2-fft', 'small', 16, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'splash2-radix', 'small',  1, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'splash2-radix', 'small',  4, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'splash2-radix', 'small', 16, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'parsec-swaptions', 'small',  4, config = ('dunnington', 'interval')),
      Test('dunnington-interval', 'parsec-swaptions', 'small', 16, config = ('dunnington', 'interval')),
    ],
    'public': [ # Shouldn't be too many commits so it won't actually run every day, but we want quick feedback
      Test('eb-2-02-interval', 'splash2-fft', 'large',  1, config = ('eb-2-02')),
      Test('eb-2-02-interval', 'splash2-fft', 'large',  8, config = ('eb-2-02')),
      Test('eb-2-02-interval', 'splash2-radix', 'large',  1, config = ('eb-2-02')),
      Test('eb-2-02-interval', 'splash2-radix', 'large',  8, config = ('eb-2-02')),
      Test('eb-2-02-interval', 'parsec-swaptions', 'large', 8, config = ('eb-2-02')),
    ],
    'stable': [
    ]
  },
  'weekly': {
    'master': [
    ],
    'stable': [
      Test('dunnington-interval', 'splash2-fft', 'small', 1, config = ('dunnington', 'interval')),
    ]
  },
}

for bm in benchmarks_splash + benchmarks_parsec + benchmarks_corevalidation:
  for arch, maxthreads in (('eb-2-02', 8),):
    for nthreads in (1, maxthreads):
      TESTS['weekly']['master'].append(Test('%s-interval' % arch.replace('-', '_'), bm, 'large', nthreads, config = (arch,)))


def usage():
  print 'Usage: %s <schedule: %s>' % (sys.argv[0], '|'.join(TESTS.keys()))
  print 'Usage: %s <branchname>' % sys.argv[0]
  sys.exit(-1)

if len(sys.argv) < 2:
  usage()


if sys.argv[1] in TESTS:
  schedule = sys.argv[1]

  for branch, tests in TESTS[schedule].items():
    gitid = ic.graphite_gitid(branch)
    if not gitid:
      print 'Graphite branch', branch, 'not valid'
      sys.exit(-1)

    if not tests:
      continue

    ic.regress_submit(schedule, branch, gitid)

    for test in tests:
      test.submit(gitid)

else:
  branch = sys.argv[1]

  gitid = ic.graphite_gitid(branch)
  if not gitid:
    print 'Invalid branch', branch
    sys.exit(-1)

  ic.regress_submit('manual', branch, gitid)

  for test in TESTS['daily']['master']:
    test.submit(gitid)
