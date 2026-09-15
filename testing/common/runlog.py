"""Per-check timing, shared by the regression suites.

`make regress` takes over an hour and the only number it reported was how many
checks passed.  Two suites timed their checks and printed `[Ns]`, two did not
time at all, and nothing added the numbers up -- so "which suite is the hour in,
and which checks inside it" could only be answered by scraping a log by hand.

Each suite keeps a RunLog, records what every check cost, and prints a one-line
total when it finishes.  When SNIPER_REGRESS_LOG names a file the records are
also appended to it as JSON lines, which is what `regress_report.py` reads to
put the suites side by side.  Unset -- a suite run on its own -- nothing is
written and only the summary line is printed.

Each record is written as it is taken, rather than held until the end.  A
suite that is killed for running too long is exactly the one whose timings are
wanted, and a SIGTERM does not run `finally`.
"""

import json
import os
import sys
import time

ENV = 'SNIPER_REGRESS_LOG'


def _fmt(seconds):
    """Seconds, in whichever unit does not make the reader do arithmetic."""
    if seconds < 90:
        return '%ds' % round(seconds)
    return '%.1f min' % (seconds / 60.0)


class RunLog(object):
    def __init__(self, suite):
        self.suite = suite
        self.started = time.time()
        self.checks = []          # (label, seconds, status)

    def record(self, label, seconds, status='PASS'):
        """Note what one check cost.  Returns the duration, to allow
        `print('... [%ds]' % round(log.record(name, took)))`."""
        self.checks.append((label, seconds, status))
        self._append({'suite': self.suite, 'check': label,
                      'seconds': round(seconds, 3), 'status': status})
        return seconds

    def _append(self, record):
        path = os.environ.get(ENV)
        if not path:
            return
        try:
            # Opened per record on purpose: nothing is then buffered in this
            # process waiting for a close that a kill will not perform.
            with open(path, 'a') as f:
                f.write(json.dumps(record) + '\n')
        except OSError as e:
            # Timing is an aid, never a reason for a regression run to fail.
            print('warning: could not write %s: %s' % (path, e), file=sys.stderr)

    def timed(self, label, status='PASS'):
        """Context manager for suites that were not timing anything yet."""
        return _Timed(self, label, status)

    def finish(self):
        """Print the suite's total and append its records to the shared log.

        Safe to call from a `finally`: a suite that died half way through still
        reports what it managed, which is the case where the timing is most
        wanted.
        """
        wall = time.time() - self.started
        inside = sum(s for _, s, _ in self.checks)
        if self.checks:
            slowest = sorted(self.checks, key=lambda c: -c[1])[:3]
            tail = ', '.join('%s %s' % (l, _fmt(s)) for l, s, _ in slowest)
            print('[%s] %d checks in %s (wall %s; slowest: %s)'
                  % (self.suite, len(self.checks), _fmt(inside), _fmt(wall), tail))
        else:
            print('[%s] no checks ran (wall %s)' % (self.suite, _fmt(wall)))

        self._append({'suite': self.suite, 'check': None,
                      'seconds': round(wall, 3), 'status': 'WALL'})


class _Timed(object):
    def __init__(self, log, label, status):
        self.log, self.label, self.status = log, label, status

    def __enter__(self):
        self.t0 = time.time()
        return self

    @property
    def seconds(self):
        return time.time() - self.t0

    def __exit__(self, *exc):
        self.log.record(self.label, time.time() - self.t0, self.status)
        return False
