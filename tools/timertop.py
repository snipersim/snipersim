#!/usr/bin/env python2

import sys, os, subprocess, addr2line

def ex(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]

if len(sys.argv) > 1:
  if sys.argv[1] == '-':
    data = sys.stdin.readlines()
  else:
    data = file(sys.argv[1]).readlines()
else:
  data = file('sim_timers.out').readlines()

addr2line.set_rdtsc(long(data[0]))

data = [ map(long, line.split()[:4]) + [ map(long, line.split()[4:10]), line.split(' ', 10)[10].strip() ] for line in data[1:] ]
data.sort(key = lambda line: line[0], reverse = True)

height, width = ex('stty size').split()
width = int(width)
if width < 120:
  width = 2*width # if we're line-wrapping anyway: use 2 full lines

for line in data[:15]:
  total, n, max, n_switched, trace, name = line
  result = '%6.1f s, %8u calls, avg %5.0f us/call, max %5.1f ms, switched = %.3f%% (%3u) ' % \
      (total / 1e9, n, total / (n or 1) / 1e3, max / 1e6, 100. * n_switched / (n or 1), n_switched)
  print '%-85s %s' % (result, name)
  for a in trace[1:6]:
    if a:
      (file, function, line) = addr2line.addr2line(a)
      function = function[:(width - 86 - len(file) - 1 - 1 - len(line))]
      print ' '*85, ':'.join((file, function, line)).strip()
