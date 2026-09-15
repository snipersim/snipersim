#!/usr/bin/env python3
"""Where the regression run spent its time.

`make regress` writes one JSON line per check to $SNIPER_REGRESS_LOG (see
common/runlog.py) and then runs this over it.  The point is to make the next
question answerable without scraping a log: which suite owns the wall clock,
and which individual checks inside it are worth attacking first.

Run it by hand over a kept log to compare two runs:

    testing/regress_report.py <log> [--slowest 20]
"""

import argparse
import collections
import json
import statistics
import sys


def fmt(seconds):
    if seconds < 90:
        return '%ds' % round(seconds)
    return '%.1f min' % (seconds / 60.0)


def bar(fraction, width=24):
    filled = int(round(fraction * width))
    return '#' * filled + '.' * (width - filled)


def load(path):
    checks, wall = [], {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue                       # a truncated final write
            if r.get('status') == 'WALL':
                wall[r['suite']] = r['seconds']
            else:
                checks.append(r)
    return checks, wall


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--slowest', type=int, default=12,
                    help='how many individual checks to list (default 12)')
    args = ap.parse_args()

    try:
        checks, wall = load(args.log)
    except OSError as e:
        print('no timing log (%s)' % e, file=sys.stderr)
        return 0                               # never fail a run over this

    if not checks and not wall:
        print('no timings recorded')
        return 0

    by_suite = collections.OrderedDict()
    for r in checks:
        s = by_suite.setdefault(r['suite'], {'n': 0, 'secs': 0.0, 'failed': 0})
        s['n'] += 1
        s['secs'] += r['seconds']
        if r['status'] not in ('PASS',):
            s['failed'] += 1
    for suite in wall:
        by_suite.setdefault(suite, {'n': 0, 'secs': 0.0, 'failed': 0})

    # Wall clock is the honest total: it includes each suite's own setup --
    # building guest binaries, compiling the workloads -- which no check owns.
    total_wall = sum(wall.values()) or sum(s['secs'] for s in by_suite.values())

    print()
    print('  %-20s %-24s %5s   %9s %9s  %6s'
          % ('runtime by suite', '', 'checks', 'in checks', 'wall', 'share'))
    print('-' * 82)
    for suite, s in sorted(by_suite.items(), key=lambda kv: -wall.get(kv[0], kv[1]['secs'])):
        w = wall.get(suite, s['secs'])
        share = w / total_wall if total_wall else 0
        print('  %-20s %-24s %5d   %9s %9s  %5.1f%%'
              % (suite, bar(share), s['n'], fmt(s['secs']), fmt(w), 100 * share))
    print('-' * 82)
    print('  %-20s %-24s %5d   %9s %9s'
          % ('total', '', sum(s['n'] for s in by_suite.values()),
             fmt(sum(s['secs'] for s in by_suite.values())), fmt(total_wall)))

    # Whether the hour is a few slow checks or the sheer count of them decides
    # what to do about it: the first is worth optimising, the second is only
    # fixed by running fewer of them, or running them at the same time.
    if checks:
        secs = sorted(r['seconds'] for r in checks)
        print()
        print('  check time: median %s, mean %s, max %s over %d checks'
              % (fmt(statistics.median(secs)), fmt(sum(secs) / len(secs)),
                 fmt(secs[-1]), len(secs)))

    slow = sorted(checks, key=lambda r: -r['seconds'])[:args.slowest]
    if slow:
        covered = sum(r['seconds'] for r in slow)
        print()
        print('slowest %d checks (%s, %.0f%% of all check time):'
              % (len(slow), fmt(covered),
                 100 * covered / max(1e-9, sum(r['seconds'] for r in checks))))
        for r in slow:
            print('  %9s  %-10s %s' % (fmt(r['seconds']), r['suite'], r['check']))

    # Setup that no check pays for is invisible in the per-check numbers and is
    # often the cheapest thing to fix, so name it rather than leave it in the
    # gap between the two totals.
    for suite, s in sorted(by_suite.items()):
        w = wall.get(suite)
        if w is None:
            continue
        overhead = w - s['secs']
        if overhead > 30 and overhead > 0.2 * w:
            print()
            print('  note: %s spent %s outside its checks (%.0f%% of the suite)'
                  % (suite, fmt(overhead), 100 * overhead / w))
    print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
