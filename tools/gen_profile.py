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
    for stack in self.children.copy():
      for k, v in prof.calls[stack].total.items():
        self.total[k] += v
      # Child is to be folded: add it to self, remove from list of children
      if prof.calls[stack].folded:
        for k, v in prof.calls[stack].data.items():
          if k != 'calls':
            self.data[k] += v
        self.children.remove(stack)
        for grandchild in prof.calls[stack].children:
          self.children.add(grandchild)
    # Fold into parents?
    self.folded = prof.foldCall(self)
  def printLine(self, prof, obj, offset = 0):
    if prof.opt_absolute:
      print >> obj, '%7d\t' % self.data['calls'] + \
                    '%9d\t' % (prof.fs_to_cycles * float(self.total['core_elapsed_time'])) + \
                    '%9d\t' % (prof.fs_to_cycles * float(self.data['core_elapsed_time'])) + \
                    '%9d\t' % self.total['instruction_count'] + \
                    '%9d\t' % self.data['instruction_count'] + \
                    '%9d\t' % self.total['l2miss'] + \
                    '  ' * offset + self.name
    else:
      print >> obj, '%7d\t' % self.data['calls'] + \
                    '%6.2f%%\t' % (100 * self.total['core_elapsed_time'] / float(prof.totals['core_elapsed_time'])) + \
                    '%6.2f%%\t' % (100 * self.data['core_elapsed_time'] / float(prof.totals['core_elapsed_time'])) + \
                    '%6.2f%%\t' % (100 * self.total['instruction_count'] / float(prof.totals['instruction_count'])) + \
                    '%7.2f\t' % (self.total['instruction_count'] / (prof.fs_to_cycles * float(self.total['core_elapsed_time']))) + \
                    '%7.2f\t' % (1000 * self.total['l2miss'] / float(self.total['instruction_count'])) + \
                    '  ' * offset + self.name
  def printTree(self, prof, obj, offset = 0):
    self.printLine(prof, obj, offset = offset)
    for stack in sorted(self.children, key = lambda stack: prof.calls[stack].total['core_elapsed_time'], reverse = True):
      if prof.calls[stack].total['core_elapsed_time'] / float(prof.totals['core_elapsed_time']) < prof.opt_cutoff:
        break
      prof.calls[stack].printTree(prof, obj, offset = offset + 1)

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
  def __init__(self, resultsdir = '.', opt_absolute = False, opt_cutoff = .001):
    self.opt_absolute = opt_absolute
    self.opt_cutoff = opt_cutoff

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
    self.headers = fp.readline().strip().split('\t')

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
        data = dict(zip(self.headers[1:], map(long, line[1:])))
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

  def foldCall(self, call):
    if call.name == '.plt':
      return True
    else:
      return False

  def write(self, obj = sys.stdout):
    if self.opt_absolute:
      print >> obj, '%7s\t%9s\t%9s\t%9s\t%9s\t%9s\t%s' % ('calls', 'cycles', 'c.self', 'icount', 'i.self', 'l2miss', 'name')
    else:
      print >> obj, '%7s\t%7s\t%7s\t%7s\t%7s\t%7s\t%s' % ('calls', 'time', 't.self', 'icount', 'ipc', 'l2.mpki', 'name')
    for stack in sorted(self.roots, key = lambda stack: self.calls[stack].total['core_elapsed_time'], reverse = True):
      self.calls[stack].printTree(self, obj = obj)

  def writeCallgrind(self, obj):
    bystatic = dict([ (fn.ieip, Category(fn.eip)) for fn in self.functions.values() ])
    for stack in self.calls:
      fn = self.functions[self.calls[stack].eip]
      bystatic[fn.ieip].add(self.calls[stack].data)
      children = {}
      for _stack in self.children[stack]:
        _ieip = self.functions[self.calls[_stack].eip].ieip
        if _ieip not in children:
          children[_ieip] = Category(self.calls[_stack].eip)
        children[_ieip].add(self.calls[_stack].total)
        children[_ieip].calls = self.calls[_stack].data['calls']
      bystatic[fn.ieip].children = children

    costs = (
      ('Cycles', 'Cycles',               lambda data: long(self.fs_to_cycles * data['core_elapsed_time'])),
      ('Calls',  'Calls',                lambda data: data['calls']),
      ('Icount', 'Instruction count',    lambda data: data['instruction_count']),
      ('L2',     'L2 load misses',       lambda data: data['l2miss']),
    )

    def formatData(data):
      return ' '.join(map(str, [ fn(data) for _, _, fn in costs ]))

    print >> obj, 'cmd: Sniper run'
    print >> obj, 'positions: instr'
    print >> obj, 'events:', ' '.join([ cost for cost, _, _ in costs ])
    for cost, desc, _ in costs:
      print >> obj, 'event: %s : %s' % (cost, desc)
    print >> obj, 'summary:', formatData(self.totals)
    print >> obj

    for site in sorted(bystatic.values(), key = lambda v: v.data.get('instruction_count',0), reverse=True):
      if not site.data:
        continue
      fn = self.functions[site.name]
      print >> obj, 'ob=%s' % fn.location[0]
      print >> obj, 'fl=%s' % fn.location[2]
      print >> obj, 'fn=%s' % fn.name
      print >> obj, '0x%x' % long(fn.ieip), formatData(site.data)
      for _site in site.children.values():
        _fn = self.functions[_site.name]
        print >> obj, 'cob=%s' % _fn.location[0]
        print >> obj, 'cfi=%s' % _fn.location[2]
        print >> obj, 'cfn=%s' % _fn.name
        print >> obj, 'calls=%s 0x%x' % (_site.calls, long(_fn.ieip))
        print >> obj, '0x%x' % long(_fn.ieip), formatData(_site.data)
      print >> obj

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

  import getopt

  def usage():
    print '%s  [-d <resultsdir (.)]  [-o <outputdir (.)]' % sys.argv[0]

  resultsdir = '.'
  outputdir = '.'

  try:
    opts, cmdline = getopt.getopt(sys.argv[1:], "hd:o:")
  except getopt.GetoptError, e:
    # print help information and exit:
    print >> sys.stderr, e
    usage()
  for o, a in opts:
    if o == '-h':
      usage()
      sys.exit()
    if o == '-d':
      resultsdir = a
    if o == '-o':
      outputdir = a

  prof = Profile(resultsdir)
  prof.write(file(os.path.join(outputdir, 'sim.profile'), 'w'))
  prof.writeCallgrind(file(os.path.join(outputdir, 'callgrind.out.sniper'), 'w'))
