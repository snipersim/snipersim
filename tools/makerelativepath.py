#!/usr/bin/env python2
# Make paths in config/graphite.py relative whenever possible

import sys, os

name, simroot, configdir = sys.argv[1:]

if configdir:
  configdir = os.path.abspath(configdir)
  if configdir.startswith(simroot):
    configdir = configdir[len(simroot):]
    if configdir and configdir[0] == '/':
      configdir = configdir[1:]

print '%s="%s"' % (name, configdir)
