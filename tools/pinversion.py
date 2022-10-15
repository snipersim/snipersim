#!/usr/bin/env python2

import sys, os, re, subprocess

def ex_ret(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]

def run_pin(pin_home):
  try:
    ver = ex_ret(pin_home+"/pin -version")
    versplit = ver.split('pin-')[1].split('\n')[0].split('-')
    print versplit[0] + '.' + versplit[1]
    return True
  except:
    return False

if len(sys.argv) > 1:
  pin_home = sys.argv[1]
else:
  pin_home = os.getenv('PIN_HOME')

headerfile = None
for filebase in ('source/include/pin/gen/cc_used_ia32_l.CVH', 'source/include/gen/cc_used_ia32_l.CVH'):
  filename = os.path.join(pin_home, filebase)
  if os.path.exists(filename):
    headerfile = filename
    break

if not headerfile:
  if not run_pin(pin_home):
    sys.exit(1)
  else:
    sys.exit(0)

version = {}
for line in file(headerfile):
  for var in ('PIN_PRODUCT_VERSION_MAJOR', 'PIN_PRODUCT_VERSION_MINOR', 'PIN_BUILD_NUMBER'):
    res = re.search('#define\s+%s\s+(\d+)' % var, line)
    if res:
      version[var] = res.group(1)
      break

print version.get('PIN_PRODUCT_VERSION_MAJOR', '?') + '.' \
    + version.get('PIN_PRODUCT_VERSION_MINOR', '?') + '.' \
    + version.get('PIN_BUILD_NUMBER', '?')
