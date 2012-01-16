#!/usr/bin/env python

import sys, os

# list of (packagename, filename)

DEPENDENCIES = [
  ('libboost-dev / boost-devel', '/usr/include/boost/lexical_cast.hpp'),
  ('libboost-filesystem-dev / boost-devel', '/usr/lib/libboost_filesystem-mt.so'),
  ('libboost-system-dev / boost-devel', '/usr/lib/libboost_system-mt.so'),
]

missing = False

for package, filename in DEPENDENCIES:
  if not os.path.exists(filename) and not os.path.exists(filename.replace('/usr/lib', '/usr/lib/x86_64-linux-gnu')):
    print '*** Please install package %s' % package
    missing = True


if missing:
  sys.exit(1)
else:
  sys.exit(0)
