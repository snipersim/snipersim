#!/usr/bin/env python

import sys, os, getopt, sniper_lib

def usage():
  print 'Usage:', sys.argv[0], '[-h (help)] [--partial <section-start>:<section-end> (default: roi-begin:roi-end)]  [<resultsdir (default: .)>]'


jobid = 0
resultsdir = '.'
partial = None

try:
  opts, args = getopt.getopt(sys.argv[1:], "hj:d:", [ "partial=" ])
except getopt.GetoptError, e:
  print e
  usage()
  sys.exit()
for o, a in opts:
  if o == '-h':
    usage()
    sys.exit()
  if o == '-d':
    resultsdir = a
  if o == '-j':
    jobid = long(a)
  if o == '--partial':
    if ':' not in a:
      sys.stderr.write('--partial=<from>:<to>\n')
      usage()
    partial = a.split(':')

if args:
  usage()
  sys.exit(-1)


results = sniper_lib.get_results(jobid, resultsdir, partial = partial)


def print_result(key, value):
  if type(value) is dict:
    for _key, _value in sorted(value.items()):
      print_result(key+'.'+_key, _value)
  else:
    print key, '=',
    if type(value) is list:
      print ', '.join(map(str, value))
    else:
      print value

for key, value in sorted(results['results'].items()):
  print_result(key, value)
