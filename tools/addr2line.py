#!/usr/bin/env python2

import sys, os, subprocess, env_setup

def ex(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]

class Addr2Line:
  def __init__(self, objname, marker_addr = 0, marker_name = '_Z5rdtscv'):
    self.bin = objname
    if marker_addr == 0:
      self.offset = 0
    else:
      base = eval('0x' + ex('nm %s | grep %s' % (self.bin, marker_name)).split()[0])
      text = eval('0x' + ex(r'objdump -h %s | grep "\.text"' % self.bin).split()[5])
      self.offset = marker_addr - base

    self.cache = {}

  def addr2line(self, a):
    a = long(a)
    if a < self.offset:
      return ('??', '??', '??')
    if a not in self.cache:
      res = ex('addr2line -Csfe %s %x' % (self.bin, a - self.offset))
      function, file = res.split('\n', 1)
      function = function.split('(')[0]
      file, line = file.split(':')
      self.cache[a] = (file, function, line.strip())
    return self.cache[a]

if __name__ == '__main__':
  if len(sys.argv) < 2:
    print 'Usage: addr2line.py <address-of-rdtsc> [<address-to-resolve> ]*'
    sys.exit(1)
  a2l = Addr2Line(os.path.join(env_setup.sim_root(), 'lib', 'pin_sim.so'), long(sys.argv[1]))
  for addr in sys.argv[2:]:
    print ':'.join(a2l(long(addr)))
