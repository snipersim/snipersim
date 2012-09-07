#!/usr/bin/env python

import sys, addr2line

if len(sys.argv) > 1:
  if sys.argv[1] == '-':
    data = sys.stdin.readlines()
  else:
    data = file(sys.argv[1]).xreadlines()
else:
  data = file('debug_backtrace.out').xreadlines()

addr2line.set_rdtsc(long(data.next()))
backtrace = data.next().split()
message = data.next()

print '-'*60
print message
print 'Backtrace:'
for addr in backtrace:
  (file, function, line) = addr2line.addr2line(addr)
  print '   ', ':'.join((file, function, line)).strip()
print '-'*60
