#!/usr/bin/env python3

import sys, addr2line, subprocess

def ex(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE, text=True).communicate()[0]


if len(sys.argv) > 1:
  if sys.argv[1] == '-':
    data = sys.stdin.readlines()
  else:
    data = open(sys.argv[1], "r")
else:
  data = open('allocations.out', "r")

addr2line.set_rdtsc(int(next(data)))

allocations = {}
for line in data:
  l = line.split()
  site, size, count = tuple(map(int, l[:-2])), int(l[-2]), int(l[-1])
  if site not in allocations:
    allocations[site] = [0, 0]
  allocations[site][0] += size
  allocations[site][1] += count

allocations = allocations.items()
allocations.sort(key = lambda site_size_count: site_size_count[1][0], reverse = True)


height, width = ex('stty size').split()
width = int(width)
if width < 120:
  width = 2*width # if we're line-wrapping anyway: use 2 full lines

print('Bytes (net)  Allocations (total)')
for site, (size, count) in allocations[:20]:
  print('%6.2fM %6dk' % (size/1024.**2, count/1000.), end=' ')
  lines = []
  for addr in site:
    if not addr: continue
    (file, function, line) = addr2line.addr2line(addr)
    if file.endswith('logmem.cc'): continue
    function = function[:(width - 8 - 8 - len(file) - 1 - 1 - len(line))]
    lines.append(':'.join((file, function, line)).strip())
  print('\n                '.join(lines))
