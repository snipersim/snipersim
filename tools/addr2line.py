#!/usr/bin/env python

import sys, os, subprocess

def ex(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]

bin = os.path.join(os.getenv('SNIPER_ROOT') or os.getenv('GRAPHITE_ROOT'), 'lib', 'pin_sim.so')
base = eval('0x' + ex('nm %s | grep _Z5rdtscv' % bin).split()[0])
text = eval('0x' + ex(r'objdump -h %s | grep "\.text"' % bin).split()[5])

def set_rdtsc(addr):
  global real, offset
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
    print 'Usage: addr2line.py --gdb [<address-of-rdtsc>]'
    print 'Usage: addr2line.py <address-of-rdtsc> [<address-to-resolve> ]*'
    sys.exit(1)
  if sys.argv[1] == '--gdb':
    if len(sys.argv) < 3:
      set_rdtsc(long(file('debug_offset.out').read()))
    else:
      set_rdtsc(long(sys.argv[2]))
    print 'add-symbol-file %s 0x%x' % (bin, text + offset)
  else:
    set_rdtsc(long(sys.argv[1]))
    for addr in sys.argv[2:]:
      print ':'.join(addr2line(long(addr)))
