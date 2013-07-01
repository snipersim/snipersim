#!/usr/bin/env python

import sys, os, subprocess, env_setup

def ex(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]

bin = os.path.join(env_setup.sim_root(), 'lib', 'pin_sim.so')
base = eval('0x' + ex('nm %s | grep _Z5rdtscv' % bin).split()[0])
text = eval('0x' + ex(r'objdump -h %s | grep "\.text"' % bin).split()[5])
bin_alt = os.path.join(env_setup.sim_root(), 'lib', 'sniper')

def set_rdtsc(addr):
  global bin, offset
  if addr == 0:
    bin = bin_alt
    offset = 0
  else:
    real = addr
    offset = real - base

addr2line_cache = {}
def addr2line(a):
  a = long(a)
  if a < offset:
    return ('??', '??', '??')
  if a not in addr2line_cache:
    res = ex('addr2line -Csfe %s %x' % (bin, a - offset))
    function, file = res.split('\n', 1)
    function = function.split('(')[0]
    file, line = file.split(':')
    addr2line_cache[a] = (file, function, line.strip())
  return addr2line_cache[a]

if __name__ == '__main__':
  if len(sys.argv) < 2:
    print 'Usage: addr2line.py <address-of-rdtsc> [<address-to-resolve> ]*'
    sys.exit(1)
  set_rdtsc(long(sys.argv[1]))
  for addr in sys.argv[2:]:
    print ':'.join(addr2line(long(addr)))
