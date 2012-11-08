#!/usr/bin/env python

import sys, os, collections

names = ('hwcontext', 'smt', 'L1-I', 'L1-D', 'L2', 'L3', 'L4', 'dram-cache', 'dram-dir', 'dram-cntlr')
ids = dict([ (name, collections.defaultdict(lambda: ' ')) for name in names ])

max_id = 0
for line in open('sim.topo'):
  name, lid, mid = line.split()
  if name not in names:
    print >> sys.stderr, 'Unknown component', name
    continue
  if lid == mid:
    ids[name][int(lid)] = 'X'
  else:
    ids[name][int(lid)] = '<'
  max_id = max(max_id, int(lid))

print ' '*20,
for lid in range(max_id+1):
  print '%3d' % lid,
print

for name in names:
  if ids[name]:
    print '%-20s' % name,
    for lid in range(max_id+1):
      print '%3s' % ids[name][lid],
    print
