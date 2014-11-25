#!/usr/bin/env python2

import os, sys, time, getopt, math, sniper_lib

def max_diff(l_notsorted):
  l = sorted(l_notsorted)
  try:
    l = map(float,l)
  except ValueError, e:
    # Values are not float (string, bool): print if more than one unique value
    l = map(str,l)
    if len(set(l)) > 1:
      return (1,100,True)
    else:
      return (0.0,0.0,False)
  except TypeError, e:
    # Some values are None (missing config): definitely print
    return (0.0,0.0,True)
  islendiff = len(l_notsorted) != len(l)
  if l[0] == 0.0:
    return (l[-1] - l[0], 0.0, islendiff)
  else:
    return (l[-1] - l[0], 100*(l[-1] / float(l[0]) - 1.0), islendiff)

def get_diffs(l):
  try:
    l = map(float,l)
  except (TypeError, ValueError), e:
    return [ _ == l[0] for _ in l[1:] ]
  if l[0] == 0:
    return [ None for _ in l[1:] ]
  else:
    return [ 100 * (_ / l[0] - 1) for _ in l[1:] ]

def group(number):
  s = '%d' % number
  groups = []
  while s and s[-1].isdigit():
    groups.append(s[-3:])
    s = s[:-3]
  return s + ','.join(reversed(groups))

def format_value(d):
  if (type(d) is long) or (type(d) is int):
    if len(group(d)) < 12:
      return '%12s' % group(d)
    else:
      d = float(d)
  if type(d) is float:
    if abs(d) > 1:
      e = 3 * math.floor(math.log10(abs(d) or 1.) / 3)
      return '%12s' % ('%.3f' % (d / 10**e) + '%+03d' % e)
    elif abs(d) > .01:
      return '%12.6f' % d
    else:
      return '%12.3e' % d
  d = str(d)
  if len(d) > 12:
    return '%12s' % (d[-11:]+'>')
  else:
    return '%12s' % d

def format_percent(d):
  if d > 500:
    return '%11.2fx' % (d / 100.)
  else:
    return '%+11.1f%%' % d

def format_diff(d):
  if d is None:
    return '        ----'
  elif d is True:
    return '          =='
  elif d is False:
    return '          !='
  else:
    return format_percent(d)

def print_diff(parmsort = None, restype = 'results', resultdirs = [], partial = None, print_alldiffs = True, print_average = False, average_nz = True):

  jobs = []
  stats = {}
  maxkeylen = -1
  resultstoprint = []
  max_cores = 0
  keys = []

  for resultdir in resultdirs:
    res = sniper_lib.get_results(resultsdir = resultdir, partial = partial)
    stats[resultdir] = res[restype]
    jobs.append(resultdir)

  # Find all key names and maximum lenghts
  def key_map((k, v)):
    return (k, len(v) if type(v) is list else 0)
  allkeys = sum([ map(key_map, s.items()) for s in stats.values() ], [])
  keyinfo = {}
  for key, length in allkeys:
    keyinfo[key] = max(keyinfo.get(key, 0), length)

  def get_element(statkey, key, core):
    data = stats[statkey].get(key)
    if data and type(data) is list and len(data) > core:
      return data[core]
    else:
      return None
  def get_average(statkey, key):
    data = stats[statkey].get(key)
    if data and type(data) is list and len(data) > 0:
      if average_nz:
        # Find cores for which this statistic is non-zero for at least one of the results
        alldata = [ stats[_statkey][key] for _statkey in stats.keys() ]
        nonzero = map(any, zip(*alldata))
        cnt = len(filter(None, nonzero)) or 1
      else:
        cnt = len(data)
      return long(sum(data) / float(cnt))
    else:
      return None

  for key, length in sorted(keyinfo.items(), key = lambda (k, v): k.lower()):
    if length > 0:
      for core in range(1 if print_average else length):
        if print_average:
          values = [ get_average(statkey, key) for statkey in jobs ]
        else:
          values = [ get_element(statkey, key, core) for statkey in jobs ]
        if any(values):
          diff, max_percent_diff, forceprint = max_diff(values)
          diffs = get_diffs(values)
          if forceprint or diff != 0:
            maxkeylen = max(len(key), maxkeylen) # Consider this key for the maximum key character length
            resultstoprint.append((key, core, values, diff, max_percent_diff, diffs))
            max_cores = max(max_cores, core)
    else:
      diff, max_percent_diff, forceprint = max_diff(map(lambda x: x.get(key, None), stats.itervalues()))
      diffs = get_diffs([ stats[statkey].get(key, None) for statkey in jobs ])
      if forceprint or diff != 0:
        maxkeylen = max(len(key), maxkeylen) # Consider this key for the maximum key character length
        data = []
        for statkey in jobs:
          try:
            data.append(stats[statkey][key])
          except KeyError:
            data.append(None)
        resultstoprint.append((key, None, data, diff, max_percent_diff, diffs))

  # Iterate through the collected data items and print them out
  print '%*s ' % (maxkeylen+5, ''),
  for statkey in jobs:
    print '%12s' % (('%s'%statkey)[-12:]),
  if print_alldiffs:
    for statkey in jobs[1:]:
      print ' '*max(0, 11 - len(str(statkey))) + u'\u0394'.encode('utf8') + str(statkey)[-11:],
  else:
    print '%12s' % 'max-%-err',
    print '%12s' % 'max-abs-err',
  print

  if parmsort == 'abs':
    resultstoprint = sorted(resultstoprint, key = lambda x: abs(x[3]), reverse = True)
  elif parmsort == 'percent':
    resultstoprint = sorted(resultstoprint, key = lambda x: abs(x[4]), reverse = True)

  for (key, core, datalist, abs_diff, percent_diff, diffs) in resultstoprint:
    if core != None:
      if print_average:
        print '%-*s[*] =' % (maxkeylen, key),
      else:
        print '%-*s[%*u] =' % (maxkeylen, key, len(str(max_cores)), core),
    else:
      print '%-*s %s  =' % (maxkeylen, key, ' '*len(str(max_cores))),
    for d in datalist:
      if d == None:
        print '        ----',
      else:
        print format_value(d),
    if print_alldiffs:
      for d in diffs:
        print format_diff(d),
    else:
      print format_percent(percent_diff),
      print '%12.3g' % abs_diff,
    print

if __name__ == "__main__":

  parmsort = None
  restype = 'results'
  resultdirs = []
  partial = None
  print_alldiffs = True
  print_average = False


  def usage():
    print 'Usage:', sys.argv[0], '[-h|--help (help)] [--sort-abs] [--sort-percent] [--max-diff] [--average] [--config] [--partial=roi-begin:roi-end] [--] [<dir> [<dirN>]]'

  try:
    opts, args = getopt.getopt(sys.argv[1:], 'h', [ 'help', 'sort-abs', 'sort-percent', 'max-diff', 'average', 'config', 'partial=' ])
  except getopt.GetoptError, e:
    print e
    usage()
    sys.exit(1)
  for o, a in opts:
    if o == '-h' or o == '--help':
      usage()
      sys.exit(1)
    if o == '--sort-abs':
      parmsort = 'abs'
    if o == '--sort-percent':
      parmsort = 'percent'
    if o == '--max-diff':
      print_alldiffs = False
    if o == '--average':
      print_average = True
    if o == '--config':
      restype = 'config'
    if o == '--partial':
      partial = tuple(a.split(':'))[0:2]

  if args:
    for arg in args:
      if os.path.isdir(arg):
        resultdirs.append(arg)
      else:
        print 'Warning: Argument [%s] is not a results directory' % arg
        pass
  else:
    print 'At least one directory is required'
    sys.exit(1)

  with sniper_lib.OutputToLess():
    print_diff(parmsort = parmsort, restype = restype, resultdirs = resultdirs, partial = partial, print_alldiffs = print_alldiffs, print_average = print_average, average_nz = True)
