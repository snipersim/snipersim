#!/usr/bin/env python

import sys, os, subprocess

def ex_ret(cmd):
  return subprocess.Popen(cmd, stdout = subprocess.PIPE).communicate()[0]
def cppfilt(name):
  return ex_ret([ 'c++filt', name ])

if len(sys.argv) > 1:
  outputdir = sys.argv[1]
else:
  outputdir = '.'

filename = os.path.join(outputdir, 'sim.rtntracefull')
if not os.path.exists(filename):
  print >> sys.stderr, 'Cannot find trace file', filename
  sys.exit(1)

fp = open(filename)
headers = fp.readline().strip().split('\t')

data = {}
functions = {}
for line in fp:
  line = dict(zip(headers, line.strip().split('\t')))
  data[line['eip']] = {'calls': long(line['calls']), 'time': long(line['core_elapsed_time'])/1e15, 'icount': long(line['instruction_count'])}
  eip = line['eip'].split(':')[-1]
  if eip not in functions:
    functions[eip] = cppfilt(line['name']).strip()

for stack in sorted(data.keys()):
  eip = stack.split(':')[-1]
  print stack, functions[eip], data[stack]['calls'], data[stack]['time'], data[stack]['icount']
