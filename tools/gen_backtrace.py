#!/usr/bin/env python2

import sys, os, env_setup, addr2line

if len(sys.argv) > 1:
  if sys.argv[1] == '-':
    data = sys.stdin.readlines()
  else:
    data = file(sys.argv[1]).xreadlines()
else:
  data = file('debug_backtrace.out').xreadlines()

objname = data.next().strip()
marker = long(data.next())
backtrace = data.next().split()
message = data.next()

if objname == 'sniper':
  bin = os.path.join(env_setup.sim_root(), 'lib', 'sniper')
elif objname == 'pin_sim.so':
  bin = os.path.join(env_setup.sim_root(), 'lib', 'pin_sim.so')
elif objname == 'sift_recorder':
  bin = os.path.join(env_setup.sim_root(), 'sift', 'recorder', 'sift_recorder')
else:
  print >> sys.stderr, 'Unknown object name', objname

a2l = addr2line.Addr2Line(bin, marker)

print '-'*60
print message
print 'Backtrace:'
for addr in backtrace:
  (file, function, line) = a2l.addr2line(addr)
  print '   ', ':'.join((file, function, line)).strip()
print '-'*60
