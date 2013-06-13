#!/usr/bin/env python

import sys, os, subprocess, sniper_lib, sniper_config

def ex_ret(cmd):
  return subprocess.Popen(cmd, stdout = subprocess.PIPE).communicate()[0]
def cppfilt(name):
  return ex_ret([ 'c++filt', name ])

if len(sys.argv) > 1:
  resultsdir = sys.argv[1]
else:
  resultsdir = '.'

filename = os.path.join(resultsdir, 'sim.rtntracefull')
if not os.path.exists(filename):
  print >> sys.stderr, 'Cannot find trace file', filename
  sys.exit(1)

config = sniper_lib.get_config(resultsdir = resultsdir)
freq = 1e9 * float(sniper_config.get_config(config, 'perf_model/core/frequency'))
fs_to_cycles_cores = freq / 1e15


functions = {}
calls = {}
roots = set()
totals = {}


class Function:
  def __init__(self, eip, name, location):
    self.eip = eip
    self.name = cppfilt(name).strip()
    self.location = location
  def __str__(self):
    return self.name
    #return '[%8s] %s' % (self.eip, self.name)

class Call:
  def __init__(self, stack, data):
    self.stack = stack
    self.data = data
    self.total = dict([ (k, 0) for k in self.data.keys() ])
    self.children = set()
  def buildTotal(self):
    for k in self.data:
      totals[k] = totals.get(k, 0) + self.data[k]
    for stack, child in calls.items():
      if stack.startswith(self.stack):
        if len(stack.split(':')) == len(self.stack.split(':')) + 1:
          self.children.add(stack)
        for k in child.data:
          self.total[k] += child.data[k]
  def printLine(self):
    print '%7d\t' % self.data['calls'] + \
          '%6.2f%%\t' % (100 * self.total['core_elapsed_time'] / float(totals['core_elapsed_time'])) + \
          '%6.2f%%\t' % (100 * self.data['core_elapsed_time'] / float(totals['core_elapsed_time'])) + \
          '%6.2f%%\t' % (100 * self.total['instruction_count'] / float(totals['instruction_count'])) + \
          '%7.2f\t' % (self.total['instruction_count'] / (fs_to_cycles_cores * float(totals['core_elapsed_time']))) + \
          '%7.2f\t' % (1000 * self.total['l2miss'] / float(self.total['instruction_count'])) + \
          '  '*len(self.stack.split(':')) + str(functions[self.stack.split(':')[-1]])
  def printTree(self):
    self.printLine()
    for stack in sorted(self.children, key = lambda stack: calls[stack].total['core_elapsed_time'], reverse = True):
      if calls[stack].total['core_elapsed_time'] / float(totals['core_elapsed_time']) < .001:
        break
      calls[stack].printTree()


fp = open(filename)
headers = fp.readline().strip().split('\t')

for line in fp:
  if line.startswith(':'):
    eip, name, location = line.strip().split('\t')
    eip = eip[1:]
    functions[eip] = Function(eip, name, location)
  else:
    line = line.strip().split('\t')
    stack = line[0]
    data = dict(zip(headers[1:], map(long, line[1:])))
    calls[stack] = Call(stack, data)

roots = set(calls.keys())
for stack in calls:
  calls[stack].buildTotal()
  roots -= calls[stack].children

print '%7s\t%7s\t%7s\t%7s\t%7s\t%7s  %s' % ('calls', 'time', 't.self', 'icount', 'ipc', 'l2.mpki', 'name')
for stack in sorted(roots, key = lambda stack: calls[stack].total['core_elapsed_time'], reverse = True):
  calls[stack].printTree()
