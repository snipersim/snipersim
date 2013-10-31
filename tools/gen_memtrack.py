#!/usr/bin/env python

import sys, os, re, collections, subprocess, sniper_lib, sniper_config


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
  def __str__(self):
    return '[%12s]  %-20s %s' % (self.eip, self.name, ':'.join(self.location))


class AllocationSite:
  def __init__(self, stack, numallocations, totalallocated, hitwhere, evictedby):
    self.stack = stack
    self.numallocations = numallocations
    self.totalallocated = totalallocated
    self.totalaccesses = sum(hitwhere.values())
    self.hitwhere = hitwhere
    self.evictedby = evictedby


def format_abs_ratio(val, tot):
  return '%12d  (%5.1f%%)' % (val, (100. * val) / tot)


class MemoryTracker:
  def __init__(self, resultsdir = '.'):
    filename = os.path.join(resultsdir, 'sim.memorytracker')
    if not os.path.exists(filename):
      raise IOError('Cannot find output file %s' % filename)

    results = sniper_lib.get_results(resultsdir = resultsdir)
    config = results['config']
    stats = results['results']

    self.hitwhere_global = dict([ (k.split('-', 3)[3], sum(v)) for k, v in stats.items() if k.startswith('L1-D.loads-where-') ])
    self.hitwhere_unknown = self.hitwhere_global.copy()

    llc_level = int(sniper_config.get_config(config, 'perf_model/cache/levels'))
    self.evicts_global = sum([ sum(v) for k, v in stats.items() if re.match('L%d.evict-.$' % llc_level, k) ])
    self.evicts_unknown = self.evicts_global

    self.functions = {}
    self.sites = {}

    fp = open(filename)
    for line in fp:
      if line.startswith('W\t'):
        self.hitwheres = line.strip().split('\t')[1].strip(',').split(',')
      elif line.startswith('F\t'):
        _, eip, name, location = line.strip().split('\t')
        self.functions[eip] = Function(eip, name, location)
      elif line.startswith('S\t'):
        line = line.strip().split('\t')
        siteid = line[1]
        stack = line[2].strip(':').split(':')
        results = { 'numallocations': 0, 'totalallocated': 0, 'hitwhere': {}, 'evictedby': {} }
        for data in line[3:]:
          key, value = data.split('=')
          if key == 'num-allocations':
            results['numallocations'] = long(value)
          if key == 'total-allocated':
            results['totalallocated'] = long(value)
          elif key == 'hit-where':
            results['hitwhere'] = dict(map(lambda (s, v): (s, long(v)), map(lambda s: s.split(':'), value.strip(',').split(','))))
            for k, v in results['hitwhere'].items():
              self.hitwhere_unknown[k] -= v
          elif key == 'evicted-by':
            results['evictedby'] = dict(map(lambda (s, v): (s, long(v)), map(lambda s: s.split(':'), value.strip(',').split(','))))
            self.evicts_unknown -= sum(results['evictedby'].values())
        self.sites[siteid] = AllocationSite(stack, **results)
      else:
        raise ValueError('Invalid format %s' % line)
    #print ', '.join([ '%s:%d' % (k, v) for k, v in hitwhere_global.items() if v ])
    #print ', '.join([ '%s:%d' % (k, v) for k, v in hitwhere_unknown.items() if v ])
    #print evicts_global, evicts_unknown

  def write(self, obj):
    for siteid, site in sorted(self.sites.items(), key = lambda (k, v): v.totalaccesses, reverse = True):
      print >> obj, 'Site %s:' % siteid
      print >> obj, '\tCall stack:'
      for eip in site.stack:
        print >> obj, '\t\t%s' % self.functions[eip]
      print >> obj, '\tAllocations: %d' % site.numallocations
      print >> obj, '\tTotal allocated: %d' % site.totalallocated
      print >> obj, '\tHit-where:'
      for hitwhere in self.hitwheres:
        if site.hitwhere.get(hitwhere):
          cnt = site.hitwhere[hitwhere]
          print >> obj, '\t\t%-15s: %s' % (hitwhere, format_abs_ratio(cnt, site.totalaccesses))
      print >> obj

    print >> obj, 'By hit-where:'
    totalaccesses = sum(self.hitwhere_global.values())
    for hitwhere in self.hitwheres:
      if self.hitwhere_global[hitwhere]:
        totalhere = self.hitwhere_global[hitwhere]
        print >> obj, '\t%-15s:  %s' % (hitwhere, format_abs_ratio(totalhere, totalaccesses))
        for siteid, site in sorted(self.sites.items(), key = lambda (k, v): v.hitwhere.get(hitwhere, 0), reverse = True):
          if site.hitwhere.get(hitwhere) > .001 * totalhere:
            print >> obj, '\t\t%12s: %s' % (siteid, format_abs_ratio(site.hitwhere.get(hitwhere), totalhere))
        if self.hitwhere_unknown.get(hitwhere) > .001 * totalhere:
          print >> obj, '\t\t%12s: %s' % ('other', format_abs_ratio(self.hitwhere_unknown.get(hitwhere), totalhere))

if __name__ == '__main__':

  import getopt

  def usage():
    print '%s  [-d <resultsdir (.)> | -o <outputdir>]' % sys.argv[0]
    sys.exit(1)

  HOME = os.path.dirname(__file__)
  resultsdir = '.'
  outputdir = None

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

  result = MemoryTracker(resultsdir)
  result.write(file(os.path.join(outputdir, 'sim.memprofile'), 'w') if outputdir else sys.stdout)
