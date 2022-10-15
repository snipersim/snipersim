#!/usr/bin/env python2

import sys, os

# list of (packagename, filename)

DEPENDENCIES = [
  ('zlib1g-dev / zlib-devel', '/usr/include/zlib.h'),
  ('libbz2-dev / bzip2-devel', '/usr/include/bzlib.h'),
  ('g++ / gcc-c++', '/usr/bin/g++'),
  ('wget', '/usr/bin/wget'),
]

if os.environ.get('BOOST_INCLUDE', ''):
  DEPENDENCIES += [
    ('boost headers to %s' % os.environ['BOOST_INCLUDE'], '%(BOOST_INCLUDE)s/boost/lexical_cast.hpp' % os.environ),
  ]
else:
  DEPENDENCIES += [
    ('libboost-dev / boost-devel', '/usr/include/boost/lexical_cast.hpp'),
  ]

if os.environ.get('SQLITE_PATH', ''):
  DEPENDENCIES += [
    ('sqlite to %s' % os.environ['SQLITE_PATH'], '%(SQLITE_PATH)s/include/sqlite3.h' % os.environ),
  ]
else:
  DEPENDENCIES += [
    ('libsqlite3-dev / sqlite-devel', '/usr/include/sqlite3.h'),
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
