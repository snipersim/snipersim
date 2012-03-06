#!/usr/bin/env python
# Create the scripts that define linker flags needed for running an app under Graphite.

import sys, os

sim_root, boost_lib, boost_suffix, pin_home, cc, cxx = sys.argv[1:]

# Needed for sim_api.h
includes = '-I${GRAPHITE_ROOT}/include'

flags = {
  'GRAPHITE_CFLAGS': '-mno-sse4 -mno-sse4.1 -mno-sse4.2 -mno-sse4a %(includes)s' % locals(), # -mno-avx
  'GRAPHITE_CXXFLAGS': '-mno-sse4 -mno-sse4.1 -mno-sse4.2 -mno-sse4a %(includes)s' % locals() , # -mno-avx
  'GRAPHITE_LDFLAGS': '-static -L${GRAPHITE_ROOT}/lib -lgraphite_user -u CarbonSpawnThreadSpawner -pthread',
  'GRAPHITE_LD_LIBRARY_PATH': '',
  'GRAPHITE_CC': cc,
  'GRAPHITE_CXX': cxx,
  'GRAPHITE_LD': cxx,
  'PIN_HOME': pin_home,
  'BOOST_LIB': boost_lib,
  'BOOST_SUFFIX': boost_suffix,
}
upcc_link = ('%(GRAPHITE_CXX)s %(GRAPHITE_LDFLAGS)s' % flags).strip()
flags['GRAPHITE_UPCCFLAGS'] = "%(includes)s -link-with='%(upcc_link)s'" % locals()

message = '# This file is auto-generated, changes made to it will be lost. Please edit %s instead.' % os.path.basename(__file__)

env_check_sh = 'if [ -z "${GRAPHITE_ROOT}" ] ; then GRAPHITE_ROOT=$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/..") ; fi'
env_check_make = 'SELF_DIR := $(dir $(lastword $(MAKEFILE_LIST)))\nGRAPHITE_ROOT ?= $(realpath $(SELF_DIR)/..)'

file('config/buildconf.sh', 'w').write('\n'.join(
  [ message, '' ] +
  [ env_check_sh, '' ] +
  [ '%s="%s"' % i for i in flags.items() ]
) + '\n')

file('config/buildconf.makefile', 'w').write('\n'.join(
  [ message, '' ] +
  [ env_check_make, '' ] +
  [ '%s:=%s' % i for i in flags.items() ]
) + '\n')
