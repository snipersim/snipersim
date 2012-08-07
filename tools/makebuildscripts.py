#!/usr/bin/env python
# Create the scripts that define linker flags needed for running an app under Graphite.

import sys, os, subprocess

sim_root, pin_home, cc, cxx, arch = sys.argv[1:]

# Needed for sim_api.h
includes = '-I${GRAPHITE_ROOT}/include'

if arch == 'intel64':
  arch_cflags = ''
  arch_ldflags = ''
elif arch == 'ia32':
  arch_cflags = '-m32'
  arch_ldflags = '-m32'
else:
  raise ValueError('Unknown architecture %s' % arch)

def determine_valid_flags():
  possible_flags = ['-mno-sse4', '-mno-sse4.1', '-mno-sse4.2', '-mno-sse4a', '-mno-avx', '-mno-avx2']
  valid_flags = []
  for flag in possible_flags:
    rc = subprocess.call([cc,'-x','c','-c','/dev/null','-o','/dev/null',flag], stdout=open('/dev/null', 'w'), stderr=subprocess.STDOUT)
    if rc == 0:
      valid_flags.append(flag)
  return ' '.join(valid_flags)

extra_cflags = determine_valid_flags()

flags = {
  'GRAPHITE_CFLAGS': '%(extra_cflags)s %(includes)s %(arch_cflags)s' % locals(),
  'GRAPHITE_CXXFLAGS': '%(extra_cflags)s %(includes)s %(arch_cflags)s' % locals(),
  'GRAPHITE_LDFLAGS': '-static -L${GRAPHITE_ROOT}/lib -pthread %(arch_ldflags)s' % locals(),
  'GRAPHITE_LD_LIBRARY_PATH': '',
  'GRAPHITE_CC': cc,
  'GRAPHITE_CXX': cxx,
  'GRAPHITE_LD': cxx,
  'PIN_HOME': pin_home,
}
upcc_link = ('%(GRAPHITE_CXX)s %(GRAPHITE_LDFLAGS)s' % flags).strip()
flags['GRAPHITE_UPCCFLAGS'] = "%(includes)s %(arch_cflags)s -link-with='%(upcc_link)s'" % locals()

message = '# This file is auto-generated, changes made to it will be lost. Please edit %s instead.' % os.path.basename(__file__)

env_check_sh = 'if [ -z "${GRAPHITE_ROOT}" ] ; then GRAPHITE_ROOT=$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/..") ; fi'
env_check_make = 'SELF_DIR := $(dir $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST)))\nGRAPHITE_ROOT ?= $(shell readlink -f $(SELF_DIR)/..)'

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
