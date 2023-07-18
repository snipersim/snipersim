#!/usr/bin/env python2

import subprocess, sys, os

# list of (packagename, filename)

EXECUTABLE_DEPENDENCIES = [
  ('g++ / gcc-c++', 'g++'),
  ('wget', 'wget'),
]

HEADER_DEPENDENCIES = [
  ('zlib1g-dev / zlib-devel', 'zlib.h'),
  ('libbz2-dev / bzip2-devel', 'bzlib.h'),
  ('libboost-dev / boost-devel', 'boost/lexical_cast.hpp'),
  ('libsqlite3-dev / sqlite-devel', 'sqlite3.h'),
]

missing = False

for package, filename in EXECUTABLE_DEPENDENCIES:
  try:
    with open(os.devnull) as devnull:
      subprocess.call(filename, stdout=devnull, stderr=devnull)
  except:
    print '*** Please install package %s' % package
    missing = True

compiler_args = ['g++', '-x', 'c++', '-E', '-']

if not missing:
  boost_include = os.environ.get('BOOST_INCLUDE', None)
  sqlite_path = os.environ.get('SQLITE_PATH', None)

  if boost_include:
    compiler_args.append('-I%s' % boost_include)

  if sqlite_path:
    compiler_args.append('-I%s/include' % sqlite_path)

with open(os.devnull, 'w') as devnull:
  for package, filename in HEADER_DEPENDENCIES:
    popen = subprocess.Popen(compiler_args, stdin=subprocess.PIPE, stdout=devnull, stderr=devnull)
    popen.stdin.write('#include <%s>' % filename)
    popen.stdin.close()
    if popen.wait():
      print ('*** Please install package %s' % package)
      missing = True


if missing:
  sys.exit(1)
else:
  sys.exit(0)
