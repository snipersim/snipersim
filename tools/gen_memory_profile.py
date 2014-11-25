#!/usr/bin/env python2

import sys, os, re, collections, subprocess, sniper_lib, sniper_config


def ex_ret(cmd):
  return subprocess.Popen(cmd, stdout = subprocess.PIPE).communicate()[0]
def cppfilt(name):
  return ex_ret([ 'c++filt', name ])


def get_source_line(filename, linenr):
  for linenum, line in enumerate(file(filename)):
    if linenum+1 == linenr:
      return line.strip()
  return ''


class Function:
  def __init__(self, eip, name, location):
    self.eip = eip
    self.name = cppfilt(name).strip()
    self.location = location.split(':')
    self.imgname = self.location[0]
    self.sourcefile = self.location[2]
    self.sourceline = long(self.location[3])
    if self.sourceline:
      self.locationshort = '%s:%d' % (os.path.basename(self.sourcefile), self.sourceline)
      if os.path.exists(self.sourcefile):
        self.locationshort += '  { %s }' % get_source_line(self.sourcefile, self.sourceline)
    else:
      self.locationshort = os.path.basename(self.imgname)
  def __str__(self):
    return '[%12s]  %-20s %s' % (self.eip, self.name, self.locationshort)


def dirSum(a, b):
  for k, v in b.items():
    a[k] = a.get(k, 0) + v
  return a


class AllocationSite:
  def __init__(self, stack, numallocations, totalallocated, hitwhereload, hitwherestore, evictedby):
    self.stack = stack
    self.numallocations = numallocations
    self.totalallocated = totalallocated
    self.totalloads = sum(hitwhereload.values())
    self.hitwhereload = hitwhereload
    self.totalstores = sum(hitwherestore.values())
    self.hitwherestore = hitwherestore
    self.evictedby = evictedby
  def update(self, numallocations, totalallocated, hitwhereload, hitwherestore, evictedby):
    self.numallocations += numallocations
    self.totalallocated += totalallocated
    self.totalloads += sum(hitwhereload.values())
    self.hitwhereload = dirSum(self.hitwhereload, hitwhereload)
    self.totalstores += sum(hitwherestore.values())
    self.hitwherestore = dirSum(self.hitwherestore, hitwherestore)
    self.evictedby = dirSum(self.evictedby, evictedby)

def format_abs_ratio(val, tot):
  if tot:
    return '%12d  (%5.1f%%)' % (val, (100. * val) / tot)
  else:
    return '%12d          ' % val


class MemoryTracker:
  def __init__(self, resultsdir = '.'):
    filename = os.path.join(resultsdir, 'sim.memorytracker')
    if not os.path.exists(filename):
      raise IOError('Cannot find output file %s' % filename)

    results = sniper_lib.get_results(resultsdir = resultsdir)
    config = results['config']
    stats = results['results']

    self.hitwhere_load_global = dict([ (k.split('-', 3)[3], sum(v)) for k, v in stats.items() if k.startswith('L1-D.loads-where-') ])
    self.hitwhere_load_unknown = self.hitwhere_load_global.copy()
    self.hitwhere_store_global = dict([ (k.split('-', 3)[3], sum(v)) for k, v in stats.items() if k.startswith('L1-D.stores-where-') ])
    self.hitwhere_store_unknown = self.hitwhere_store_global.copy()

    llc_level = int(sniper_config.get_config(config, 'perf_model/cache/levels'))
    self.evicts_global = sum([ sum(v) for k, v in stats.items() if re.match('L%d.evict-.$' % llc_level, k) ])
    self.evicts_unknown = self.evicts_global

    self.functions = {}
    self.sites = {}
    self.siteids = {}

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
        stack = self.collapseStack(stack)
        results = { 'numallocations': 0, 'totalallocated': 0, 'hitwhereload': {}, 'hitwherestore': {}, 'evictedby': {} }
        for data in line[3:]:
          key, value = data.split('=')
          if key == 'num-allocations':
            results['numallocations'] = long(value)
          if key == 'total-allocated':
            results['totalallocated'] = long(value)
          elif key == 'hit-where':
            entries = map(lambda s: s.split(':'), value.strip(',').split(','))
            results['hitwhereload'] = dict([ (s[1:], long(v)) for s, v in entries if s.startswith('L') ])
            for k, v in results['hitwhereload'].items():
              self.hitwhere_load_unknown[k] -= v
            results['hitwherestore'] = dict([ (s[1:], long(v)) for s, v in entries if s.startswith('S') ])
            for k, v in results['hitwherestore'].items():
              self.hitwhere_store_unknown[k] -= v
          elif key == 'evicted-by':
            results['evictedby'] = dict(map(lambda (s, v): (s, long(v)), map(lambda s: s.split(':'), value.strip(',').split(','))))
            self.evicts_unknown -= sum(results['evictedby'].values())
        self.siteids[siteid] = stack
        if stack in self.sites:
          self.sites[stack].update(**results)
        else:
          self.sites[stack] = AllocationSite(stack, **results)
      else:
        raise ValueError('Invalid format %s' % line)
    #print ', '.join([ '%s:%d' % (k, v) for k, v in hitwhere_global.items() if v ])
    #print ', '.join([ '%s:%d' % (k, v) for k, v in hitwhere_unknown.items() if v ])
    #print evicts_global, evicts_unknown

  def collapseStack(self, stack):
    _stack = []
    for eip in stack:
      if eip == '0':
        continue
      name = self.functions[eip].name if eip in self.functions else '(unknown)'
      if name in ('.plt',):
        continue
      if name.startswith('_dl_'):
        continue
      _stack.append(eip)
    return tuple(_stack)

  def write(self, obj):
    sites_sorted = sorted(self.sites.items(), key = lambda (k, v): v.totalloads + v.totalstores, reverse = True)
    site_names = dict([ (stack, '#%d' % (idx+1)) for idx, (stack, site) in enumerate(sites_sorted) ])
    totalloads = sum(self.hitwhere_load_global.values())
    totalstores = sum(self.hitwhere_store_global.values())

    for stack, site in sites_sorted:
      print >> obj, 'Site %s:' % site_names[stack]
      print >> obj, '\tCall stack:'
      for eip in site.stack:
        print >> obj, '\t\t%s' % (self.functions[eip] if eip in self.functions else '(unknown)')
      print >> obj, '\tAllocations: %d' % site.numallocations
      print >> obj, '\tTotal allocated: %s (%s average)' % (sniper_lib.format_size(site.totalallocated), sniper_lib.format_size(site.totalallocated / site.numallocations))

      print >> obj, '\tHit-where:'
      print >> obj, '\t\t%-15s: %s' % ('Loads', format_abs_ratio(site.totalloads, totalloads)),
      print >> obj, '\t%-15s: %s' % ('Stores', format_abs_ratio(site.totalstores, totalstores))
      for hitwhere in self.hitwheres:
        if site.hitwhereload.get(hitwhere) or site.hitwherestore.get(hitwhere):
          cnt = site.hitwhereload[hitwhere]
          print >> obj, '\t\t  %-15s: %s' % (hitwhere, format_abs_ratio(cnt, site.totalloads)),
          cnt = site.hitwherestore[hitwhere]
          print >> obj, '\t  %-15s: %s' % (hitwhere, format_abs_ratio(cnt, site.totalstores))

      print >> obj, '\tEvicts:'
      evicts = {}
      for _stack, _site in self.sites.items():
        for _siteid, _cnt in _site.evictedby.items():
          if self.siteids.get(_siteid) == stack:
            evicts[_stack] = evicts.get(_stack, 0) + _cnt
      evicts = sorted(evicts.items(), key = lambda (_stack, _cnt): _cnt, reverse = True)
      for _stack, cnt in evicts[:10]:
        name = site_names.get(_stack, 'other') if _stack != stack else 'self'
        print >> obj, '\t\t%-15s: %12d' % (name, cnt)

      print >> obj, '\tEvicted-by:'
      evicts = {}
      for siteid, cnt in site.evictedby.items():
        _stack = self.siteids[siteid] if siteid != '0' else 'other'
        evicts[_stack] = evicts.get(_stack, 0) + cnt
      evicts = sorted(evicts.items(), key = lambda (stack, cnt): cnt, reverse = True)
      for _stack, cnt in evicts[:10]:
        name = site_names.get(_stack, 'other') if _stack != stack else 'self'
        print >> obj, '\t\t%-15s: %12d' % (name, cnt)
      print >> obj

    print >> obj, 'By hit-where:'
    for hitwhere in self.hitwheres:
      if self.hitwhere_load_global[hitwhere] + self.hitwhere_store_global[hitwhere]:
        totalloadhere = self.hitwhere_load_global[hitwhere]
        totalstorehere = self.hitwhere_store_global[hitwhere]
        print >> obj, '\t%s:' % hitwhere
        print >> obj, '\t\t%-15s: %s' % ('Loads', format_abs_ratio(totalloadhere, totalloads)),
        print >> obj, '\t%-15s: %s' % ('Stores', format_abs_ratio(totalstorehere, totalstores))
        for stack, site in sorted(self.sites.items(), key = lambda (k, v): v.hitwhereload.get(hitwhere, 0) + v.hitwherestore.get(hitwhere, 0), reverse = True):
          if site.hitwhereload.get(hitwhere) > .001 * totalloadhere or site.hitwherestore.get(hitwhere) > .001 * totalstorehere:
            print >> obj, '\t\t  %-15s: %s' % (site_names[stack], format_abs_ratio(site.hitwhereload.get(hitwhere), totalloadhere)),
            print >> obj, '\t  %-15s: %s' % (site_names[stack], format_abs_ratio(site.hitwherestore.get(hitwhere), totalstorehere))
        if self.hitwhere_load_unknown.get(hitwhere) > .001 * totalloadhere or self.hitwhere_store_unknown.get(hitwhere) > .001 * totalstorehere:
          print >> obj, '\t\t  %-15s: %s' % ('other', format_abs_ratio(self.hitwhere_load_unknown.get(hitwhere), totalloadhere)),
          print >> obj, '\t  %-15s: %s' % ('other', format_abs_ratio(self.hitwhere_store_unknown.get(hitwhere), totalstorehere))

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
  result.write(file(os.path.join(outputdir, 'sim.memoryprofile'), 'w') if outputdir else sys.stdout)
