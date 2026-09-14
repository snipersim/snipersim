#!/usr/bin/env python3
#
# Report Pac-Sim's estimated run time for a directory of Sniper output.
#
# This is the after-the-fact path, for a run that has already finished or one
# whose output you were handed.  The normal way to get this number is to ask
# for it when the run is made:
#
#     run-sniper --pacsim -- ./app
#
# which prints the same estimate at the end, computed by the same function.
# There is deliberately no second implementation here: both call
# pacsimscripts.mtng_dumpjson.get_full_time(), so the two paths cannot report
# different numbers for the same directory.

import argparse
import os
import sys

from pacsimscripts import mtng_dumpjson


def report(outputdir, sniper_root, parallel=False):
    sim_time, wall_time, l2_miss_rate = mtng_dumpjson.get_full_time(
        outputdir, sniper_root, parallel)
    return sim_time, wall_time, l2_miss_rate


def main():
    ap = argparse.ArgumentParser(
        description="Report Pac-Sim's estimated run time for a Sniper output directory. "
                    'Prefer run-sniper --pacsim, which prints the same estimate at the '
                    'end of the run.')
    ap.add_argument('-d', '--outputdir', default='./tmp/',
                    help='Sniper output directory to read (default: %(default)s)')
    ap.add_argument('--sniper-root', default=os.environ.get('SNIPER_ROOT'),
                    help='Sniper checkout (default: $SNIPER_ROOT)')
    ap.add_argument('--parallel', action='store_true',
                    help='report wall time for a parallel run')
    args = ap.parse_args()

    if not args.sniper_root:
        print('pacsim.py: SNIPER_ROOT is not set and --sniper-root was not given',
              file=sys.stderr)
        return 2
    if not os.path.isdir(args.outputdir):
        print('pacsim.py: no such output directory: %s' % args.outputdir,
              file=sys.stderr)
        return 2

    sim_time, wall_time, l2_miss_rate = report(
        args.outputdir, args.sniper_root, args.parallel)
    print('[PacSim] Estimated sim time %.3fs, wall time %.3fs, L2 miss rate %.4f'
          % (sim_time, wall_time, l2_miss_rate))
    return 0


if __name__ == '__main__':
    sys.exit(main())
