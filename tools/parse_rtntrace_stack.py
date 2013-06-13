#!/usr/bin/env python

import sys, os, collections, subprocess, sniper_lib, sniper_config

def ex_ret(cmd):
  return subprocess.Popen(cmd, stdout = subprocess.PIPE).communicate()[0]
def cppfilt(name):
  return ex_ret([ 'c++filt', name ])

class Function:
  def __init__(self, eip, name, location):
    self.eip = eip
    self.name = cppfilt(name).strip()
    self.location = location.split(':')
    self.img = self.location[0]
    self.offset = long(self.location[1])
    # link-time address
    self.ieip = str(long(eip, 16) - self.offset)
  def __str__(self):
    return self.name
    #return '[%8s] %s' % (self.eip, self.name)

class Call:
  def __init__(self, name, eip, stack, data):
    self.name = name
    self.eip = eip
    self.stack = stack
    self.data = data
  def add(self, data):
    for k, v in data.items():
      self.data[k] = self.data.get(k, 0) + v
  def buildTotal(self, prof):
    self.children = prof.children[self.stack]
    # Add self to global total
    for k, v in self.data.items():
      prof.totals[k] = prof.totals.get(k, 0) + v
	# Calculate children's totals
    for stack in self.children:
      prof.calls[stack].buildTotal(prof)
    # Add all children to our total
    self.total = dict(self.data)
    for stack in self.children:
      for k, v in prof.calls[stack].total.items():
        self.total[k] += v
  def printLine(self, obj, offset = 0):
    print >> obj, '%7d\t' % self.data['calls'] + \
                  '%6.2f%%\t' % (100 * self.total['core_elapsed_time'] / float(prof.totals['core_elapsed_time'])) + \
                  '%6.2f%%\t' % (100 * self.data['core_elapsed_time'] / float(prof.totals['core_elapsed_time'])) + \
                  '%6.2f%%\t' % (100 * self.total['instruction_count'] / float(prof.totals['instruction_count'])) + \
                  '%7.2f\t' % (self.total['instruction_count'] / (prof.fs_to_cycles * float(self.total['core_elapsed_time']))) + \
                  '%7.2f\t' % (1000 * self.total['l2miss'] / float(self.total['instruction_count'])) + \
                  '  '*(len(self.stack.split(':')) + offset) + self.name
  def printTree(self, prof, obj, offset = 0):
    self.printLine(obj, offset = offset)
    for stack in sorted(self.children, key = lambda stack: prof.calls[stack].total['core_elapsed_time'], reverse = True):
      if prof.calls[stack].total['core_elapsed_time'] / float(prof.totals['core_elapsed_time']) < .001:
        break
      prof.calls[stack].printTree(prof, obj, offset = offset)

class Category(Call):
  def __init__(self, name):
    self.name = name
    self.stack = ''
    self.data = {}
  def printLine(self, prof, obj):
    print >> obj, '%6.2f%%\t' % (100 * self.data['core_elapsed_time'] / float(prof.totals['core_elapsed_time'])) + \
                  '%6.2f%%\t' % (100 * self.data['instruction_count'] / float(prof.totals['instruction_count'])) + \
                  '%7.2f\t' % (self.data['instruction_count'] / (prof.fs_to_cycles * float(self.data['core_elapsed_time']))) + \
                  '%7.2f\t' % (1000 * self.data['l2miss'] / float(self.data['instruction_count'])) + \
                  self.name

class Profile:
  def __init__(self, resultsdir = '.'):
    filename = os.path.join(resultsdir, 'sim.rtntracefull')
    if not os.path.exists(filename):
      raise IOError('Cannot find trace file %s' % filename)

    config = sniper_lib.get_config(resultsdir = resultsdir)
    freq = 1e9 * float(sniper_config.get_config(config, 'perf_model/core/frequency'))
    self.fs_to_cycles = freq / 1e15

    self.functions = {}
    self.calls = {}
    self.children = collections.defaultdict(set)
    self.roots = set()
    self.totals = {}

    fp = open(filename)
    headers = fp.readline().strip().split('\t')

    for line in fp:
      if line.startswith(':'):
        eip, name, location = line.strip().split('\t')
        eip = eip[1:]
        self.functions[eip] = Function(eip, name, location)
      else:
        line = line.strip().split('\t')
        stack = line[0].split(':')
        eip = stack[-1]
        stack = ':'.join(map(self.translateEip, stack))
        data = dict(zip(headers[1:], map(long, line[1:])))
        if stack in self.calls:
          self.calls[stack].add(data)
        else:
          self.calls[stack] = Call(str(self.functions[eip]), eip, stack, data)
          parent = stack.rpartition(':')[0]
          self.children[parent].add(stack)

    self.roots = set(self.calls.keys())
    for parent in self.calls:
      for child in self.children[parent]:
        self.roots.remove(child)

    for stack in self.roots:
      self.calls[stack].buildTotal(self)

  def translateEip(self, eip):
    if eip in self.functions:
      return self.functions[eip].ieip
    else:
      return eip

  def write(self, obj = sys.stdout):
    print >> obj, '%7s\t%7s\t%7s\t%7s\t%7s\t%7s\t%s' % ('calls', 'time', 't.self', 'icount', 'ipc', 'l2.mpki', 'name')
    for stack in sorted(self.roots, key = lambda stack: self.calls[stack].total['core_elapsed_time'], reverse = True):
      self.calls[stack].printTree(self, obj = obj, offset = -len(stack.split(':')))

  def summarize(self, catnames, catfilters, obj = sys.stdout):
    bytype = dict([ (name, Category(name)) for name in catnames ])
    for func in self.calls.values():
      for catname, catfilter in catfilters:
        if catfilter(func, self):
          break
      bytype[catname].add(func.data)
    print >> obj, '%7s\t%7s\t%7s\t%7s' % ('time', 'icount', 'ipc', 'l2.mpki')
    for name in catnames:
      if bytype[name].data:
        bytype[name].printLine(self, obj = obj)


if __name__ == '__main__':

  if len(sys.argv) > 1:
    resultsdir = sys.argv[1]
  else:
    resultsdir = '.'

  prof = Profile(resultsdir)
  prof.write()
