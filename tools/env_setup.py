#!//usr/bin/env python

import os, sys


def local_sniper_root():
  return os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def sniper_root():
  # Return an existing *_ROOT if it is valid
  for rootname in ('SNIPER_ROOT', 'GRAPHITE_ROOT'):
    root = os.getenv(rootname)
    if root:
      if not os.path.isfile(os.path.join(root,'run-sniper')):
	print >> sys.stderr, 'Error: %s does not appear to be valid [%s]' % (rootname, root)
      else:
	if os.path.realpath(root) != local_sniper_root():
	  print >> sys.stderr, 'Warning: %s is different from current script directory [%s]!=[%s]' % (rootname, os.path.realpath(root), local_sniper_root())
	return root

  # Use the root corresponding to this file when nothing has been set
  return local_sniper_root()


def sim_root():
  return sniper_root()


def benchmarks_root():
  # Return an existing BENCHMARKS_ROOT if it is valid
  if os.getenv('BENCHMARKS_ROOT'):
    if os.path.isfile(os.path.join(os.getenv('BENCHMARKS_ROOT'),'run-sniper')):
      return os.getenv('BENCHMARKS_ROOT')
    else:
      print >> sys.stderr, 'Warning: BENCHMARKS_ROOT does not appear to be valid [%s]' % os.getenv('BENCHMARKS_ROOT')

  # Try to determine what the BENCHMARKS_ROOT should be if it is not set
  benchtry = []
  benchtry.append(os.path.realpath(os.path.join(sniper_root(),'..','benchmarks','run-sniper')))
  benchtry.append(os.path.realpath(os.path.join(sniper_root(),'benchmarks','run-sniper')))
  benchtry.append(os.path.realpath(os.path.join(sniper_root(),'..','run-sniper')))

  for bt in benchtry:
    if os.path.isfile(bt):
      return os.path.dirname(bt)

  return None


if __name__ == "__main__":
    print sniper_root(), benchmarks_root()
