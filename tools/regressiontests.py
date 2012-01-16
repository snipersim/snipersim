#!/usr/bin/env python

import sys, os
sys.path.append(os.path.join(os.getenv('BENCHMARKS_ROOT'), 'tools', 'scheduler'))
import intelqueue, iqclient, iqlib
ic = iqclient.IntelClient()

JOBGROUP = 'regress'

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

for bm in 'barnes cholesky fft fmm lu.cont lu.ncont ocean.cont ocean.ncont radiosity radix raytrace raytrace_opt volrend water.nsq water.sp'.split():
  bm = 'splash2-%s' % bm
  for arch, maxthreads in (('dunnington16', 16), ('eb-2-02', 8)):
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
