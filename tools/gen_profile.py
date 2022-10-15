#!/usr/bin/env python2

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
    # Assumes all children have already been visited!
    self.children = prof.children[self.stack]
    # Add self to global total
    for k, v in self.data.items():
      prof.totals[k] = prof.totals.get(k, 0) + v
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


class Category(Call):
  def __init__(self, name):
    self.name = name
    self.stack = ''
    self.data = {}
  def printLine(self, prof, obj):
    print >> obj, '%6.2f%%\t' % (100 * self.data['nonidle_elapsed_time'] / float(prof.totals['nonidle_elapsed_time'])) + \
                  '%6.2f%%\t' % (100 * self.data['instruction_count'] / float(prof.totals['instruction_count'])) + \
                  '%7.2f\t' % (self.data['instruction_count'] / (prof.fs_to_cycles * float(self.data['nonidle_elapsed_time']))) + \
                  '%7.2f\t' % (1000 * self.data['l2miss'] / float(self.data['instruction_count'])) + \
                  self.name


class CallPrinter:
  def __init__(self, prof, obj, opt_cutoff):
    self.prof = prof
    self.obj = obj
    self.opt_cutoff = opt_cutoff
  def printTree(self, stack, offset = 0):
    call = self.prof.calls[stack]
    self.printLine(call, offset = offset)
    for child in sorted(call.children, key = lambda stack: self.prof.calls[stack].total['nonidle_elapsed_time'], reverse = True):
      child_time = self.prof.calls[child].total['nonidle_elapsed_time'] + self.prof.calls[child].total['waiting_cost']
      if child_time / float(self.prof.totals['nonidle_elapsed_time']) < self.opt_cutoff:
        break
      self.printTree(child, offset = offset + 1)


class CallPrinterDefault(CallPrinter):
  def printHeader(self):
    print >> self.obj, '%7s\t%7s\t%7s\t%7s\t%7s\t%7s\t%7s\t%s' % ('calls', 'time', 't.self', 't.wait', 'icount', 'ipc', 'l2.mpki', 'name')
  def printLine(self, call, offset):
    print >> self.obj, '%7d\t' % call.data['calls'] + \
                       '%6.2f%%\t' % (100 * call.total['nonidle_elapsed_time'] / float(self.prof.totals['nonidle_elapsed_time'] or 1)) + \
                       '%6.2f%%\t' % (100 * call.data['nonidle_elapsed_time'] / float(self.prof.totals['nonidle_elapsed_time'] or 1)) + \
                       '%6.2f%%\t' % (100 * call.data['waiting_cost'] / float(self.prof.totals['total_coretime'] or 1)) + \
                       '%6.2f%%\t' % (100 * call.total['instruction_count'] / float(self.prof.totals['instruction_count'] or 1)) + \
                       '%7.2f\t' % (call.total['instruction_count'] / (self.prof.fs_to_cycles * float(call.total['nonidle_elapsed_time'] or 1))) + \
                       '%7.2f\t' % (1000 * call.total['l2miss'] / float(call.total['instruction_count'] or 1)) + \
                       '  ' * offset + call.name


class CallPrinterAbsolute(CallPrinter):
  def printHeader(self):
    print >> self.obj, '%7s\t%9s\t%9s\t%9s\t%9s\t%9s\t%9s\t%s' % ('calls', 'cycles', 'c.self', 'c.wait', 'icount', 'i.self', 'l2miss', 'name')
  def printLine(self, call, offset):
    print >> self.obj, '%7d\t' % call.data['calls'] + \
                       '%9d\t' % (self.prof.fs_to_cycles * float(call.total['nonidle_elapsed_time'])) + \
                       '%9d\t' % (self.prof.fs_to_cycles * float(call.data['nonidle_elapsed_time'])) + \
                       '%9d\t' % (self.prof.fs_to_cycles * float(call.data['waiting_cost'])) + \
                       '%9d\t' % call.total['instruction_count'] + \
                       '%9d\t' % call.data['instruction_count'] + \
                       '%9d\t' % call.total['l2miss'] + \
                       '  ' * offset + call.name


class Profile:
  def __init__(self, resultsdir = '.'):
    filename = os.path.join(resultsdir, 'sim.rtntracefull')
    if not os.path.exists(filename):
      raise IOError('Cannot find trace file %s' % filename)

    results = sniper_lib.get_results(resultsdir = resultsdir)
    config = results['config']
    stats = results['results']
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

    # Construct a list of calls where each child is ordered before its parent.
    calls_ordered = collections.deque()
    calls_tovisit = collections.deque(self.roots)
    while calls_tovisit:
      stack = calls_tovisit.pop()
      calls_ordered.appendleft(stack)
      calls_tovisit.extend(self.children[stack])
    # Now implement a non-recursive version of buildTotal, which requires that each
    # function's children have been visited before processing the parent,
    # by visiting calls_ordered in left-to-right order.
    for stack in calls_ordered:
      self.calls[stack].buildTotal(self)

    ncores = int(config['general/total_cores'])
    self.totals['total_coretime'] = ncores * stats['barrier.global_time'][0]

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

  def write(self, obj = sys.stdout, opt_absolute = False, opt_cutoff = .001):
    if opt_absolute:
      printer = CallPrinterAbsolute(self, obj, opt_cutoff = opt_cutoff)
    else:
      printer = CallPrinterDefault(self, obj, opt_cutoff = opt_cutoff)
    printer.printHeader()
    for stack in sorted(self.roots, key = lambda stack: self.calls[stack].total['nonidle_elapsed_time'], reverse = True):
      printer.printTree(stack)

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
      ('Cycles', 'Cycles',               lambda data: long(self.fs_to_cycles * data['nonidle_elapsed_time'])),
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
    def get_catname(func):
      stack = func.stack
      while stack:
        has_parent = (':' in stack)
        # Find category for this function by trying a match against all filters in catfilters
        for catname, catfilter in catfilters:
          if catfilter(self.calls[stack], self):
            if catname:
              return catname
            elif has_parent:
              # catname == None means fold into the parent
              # break out of this for loop, and visit parent function
              break
            else:
              # Ignore fold matches for root functions, try to match with another category
              continue
        # Visit parent function
        stack = stack.rpartition(':')[0]
    bytype = dict([ (name, Category(name)) for name in catnames ])
    for func in self.calls.values():
      if not func.folded:
        catname = get_catname(func)
        bytype[catname].add(func.data)
    print >> obj, '%7s\t%7s\t%7s\t%7s' % ('time', 'icount', 'ipc', 'l2.mpki')
    for name in catnames:
      if bytype[name].data:
        bytype[name].printLine(self, obj = obj)


if __name__ == '__main__':

  import getopt

  def usage():
    print '%s  [-d <resultsdir (.)> | -o <outputdir>] [--abs]' % sys.argv[0]
    sys.exit(1)

  HOME = os.path.dirname(__file__)
  resultsdir = '.'
  outputdir = None
  opt_absolute = False

  try:
    opts, cmdline = getopt.getopt(sys.argv[1:], "hd:o:", ['abs'])
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
    if o == '--abs':
      opt_absolute = True

  prof = Profile(resultsdir)
  prof.write(file(os.path.join(outputdir, 'sim.profile'), 'w') if outputdir else sys.stdout, opt_absolute = opt_absolute)

  if outputdir:
    callgrindfile = os.path.join(outputdir, 'callgrind.out.sniper')
    prof.writeCallgrind(file(callgrindfile, 'w'))

    gprof2dot_py = os.path.join(HOME, 'gprof2dot.py')
    dotbasefile = os.path.join(outputdir, 'sim.profile')
    os.system('%s --format=callgrind --output=%s.dot %s' % (gprof2dot_py, dotbasefile, callgrindfile))
    import distutils.spawn
    if distutils.spawn.find_executable('dot'):
      os.system('dot -Tpng %s.dot -o %s.png' % (dotbasefile, dotbasefile))
      os.system('dot -Tsvg %s.dot -o %s.svg' % (dotbasefile, dotbasefile))
