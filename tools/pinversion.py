#!/usr/bin/env python

import sys, os, re

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
  sys.exit(1)

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
