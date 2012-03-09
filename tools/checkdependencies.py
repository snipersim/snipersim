#!/usr/bin/env python

import sys, os

# list of (packagename, filename)

DEPENDENCIES = [
  ('zlib-dev / zlib-devel', '/usr/include/zlib.h'),
]

if os.environ.get('BOOST_INCLUDE', ''):
  DEPENDENCIES = [
    ('boost headers to %s' % os.environ['BOOST_INCLUDE'], '%(BOOST_INCLUDE)s/boost/lexical_cast.hpp' % os.environ),
    ('libboost-filesystem to %s' % os.environ['BOOST_LIB'], '%(BOOST_LIB)s/libboost_filesystem-%(BOOST_SUFFIX)s.so' % os.environ),
    ('libboost-system to %s' % os.environ['BOOST_LIB'], '%(BOOST_LIB)s/libboost_system-%(BOOST_SUFFIX)s.so' % os.environ),
  ]
else:
  DEPENDENCIES = [
    ('libboost-dev / boost-devel', '/usr/include/boost/lexical_cast.hpp'),
    ('libboost-filesystem-dev / boost-devel', '/usr/lib/libboost_filesystem-mt.so'),
    ('libboost-system-dev / boost-devel', '/usr/lib/libboost_system-mt.so'),
  ]

ALTERNATIVES = [
  ('/usr/lib', '/usr/lib/x86_64-linux-gnu'),
  ('/usr/lib', '/usr/lib64'),
]

missing = False

def find_file(filename):
  if os.path.exists(filename):
    return True
  for pattern, replacement in ALTERNATIVES:
    if os.path.exists(filename.replace(pattern, replacement)):
      return True
  return False

for package, filename in DEPENDENCIES:
  if not find_file(filename):
    print '*** Please install package %s' % package
    missing = True


if missing:
  sys.exit(1)
else:
  sys.exit(0)
