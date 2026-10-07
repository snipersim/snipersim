#!/usr/bin/env python3
"""
Regression tests for Sniper's embedded Python scripting hooks.

Runs a multi-threaded workload under each hook script and checks that the
simulation completes, the application produces the right answer, and no Python
exception escaped.  Threads matter here: the interpreter Sniper embeds today has
them, so hook callbacks cross the GIL from simulator threads.  A missing
PyGILState_Ensure() on any of those paths shows up as a segfault, and only under
load -- inspection will not find it.

Each case also checks what its script is documented to produce: the lines it
prints, the file it writes into the output directory, the markers or the
statistics snapshots it records, or the statistic its action leaves behind.
Scripts that end the simulation early (stop-by-time, stop-by-icount-percore)
are not expected to let the application finish, and say so.  Scripts whose
real trigger the smoke workload cannot provide (a SimUser command from the
application, a thread criticality query) are still run, for a clean exit and
whatever they do print, with the limitation noted at the case.

Usage:
  python3 testing/test_scripting.py                 # all cases
  python3 testing/test_scripting.py --case statscb  # one case
  python3 testing/test_scripting.py --repeat 5      # shake out races
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
APP_SRC = os.path.join(HERE, 'apps', 'threaded.c')

# The workload prints this once it has joined every thread.  Any corruption of
# simulated state shows up as a different checksum or no line at all.
EXPECTED_APP_OUTPUT = re.compile(r'threads=4 total=37\.589880')

# Cores must exceed the thread count, and the static scheduler refuses to place
# more threads than it has free cores, so pin instead.
SNIPER_ARGS = [
    '-n', '8',
    '-caddress_translation_schemes/baseline',
    '-g', '--scheduler/type=pinned',
]

# The instruction-count hook (sim.util.EveryIns, behind periodicins-stats and
# stop-by-icount-percore) fires every core/hook_periodic_ins/ins_global
# aggregate instructions, 1M by default.  The workload runs about 0.8M in
# all, so at the default the hook never fires and both scripts sit silent;
# this brings the period under the workload.
INS_HOOK_ARGS = ['-g', '--core/hook_periodic_ins/ins_global=100000',
                 '-g', '--core/hook_periodic_ins/ins_per_core=1000']


def read_stats(outdir):
    sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
    import sniper_lib
    return sniper_lib.get_results(resultsdir=outdir)['results']


def stats_db(outdir):
    sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
    import sniper_stats_sqlite
    return sniper_stats_sqlite.SniperStatsSqlite(
        os.path.join(outdir, 'simulation', 'sim.stats.sqlite3'))


def file_has_lines(name, minimum):
    """Check builder: <outdir>/<name> exists and holds at least `minimum` lines."""
    def check(outdir, out):
        path = os.path.join(outdir, name)
        if not os.path.exists(path):
            return False, '%s not written' % name
        n = len(open(path).read().splitlines())
        if n < minimum:
            return False, '%s has %d lines, expected at least %d' % (name, n, minimum)
        return True, '%s: %d lines' % (name, n)
    return check


def dvfs_took_effect(outdir, out):
    """dvfs.py prints nothing; the frequency change it makes on core 1 leaves
    the DVFS transition cost in that core's CPI stack, zero on every other
    core and in every run without the script."""
    stats = read_stats(outdir)
    cost = stats.get('performance_model.cpiSyncDvfsTransition')
    if not cost or len(cost) < 2:
        return False, 'performance_model.cpiSyncDvfsTransition not registered'
    if cost[1] <= 0:
        return False, 'no DVFS transition on core 1 (cost %s)' % cost
    others = [c for i, c in enumerate(cost) if i != 1 and c]
    if others:
        return False, 'DVFS transition on a core the script never touched: %s' % cost
    return True, 'core 1 transition cost %d fs' % cost[1]


def markers_hold_output(outdir, out):
    """output-as-markers records each write(2) to stdout/stderr as a marker
    with the arguments as its two values and the prefix in its string."""
    marks = stats_db(outdir).get_markers()
    hits = [m for m in marks if m[3] == 1 and m[4] == 2
            and m[5].startswith('appstdout: ') and EXPECTED_APP_OUTPUT.search(m[5])]
    if not hits:
        return False, 'no marker carrying the application output (%d markers)' % len(marks)
    return True, '%d markers, application output in one' % len(marks)


def periodicins_snapshots(outdir, out):
    """periodicins-stats writes a statistics snapshot named periodicins-<N>
    at each instruction-count period."""
    snaps = [p for p in stats_db(outdir).get_snapshots() if p.startswith('periodicins-')]
    # 0.8M instructions at a 100k period: eight, give or take the hook's granularity.
    if len(snaps) < 5:
        return False, 'only %d periodicins snapshots: %s' % (len(snaps), snaps)
    return True, '%d snapshots' % len(snaps)


def case(script, expect=(), args=(), app_output=True, check=None):
    """A case: the -s argument (None for no script), the regexes the output
    must match, extra run-sniper arguments, whether the application is
    expected to run to completion, and a check(outdir, out) on the results."""
    if isinstance(expect, str):
        expect = (expect,)
    return {'script': script, 'expect': tuple(expect), 'args': list(args),
            'app_output': app_output, 'check': check}


CASES = {
    'statscb':        case('statscb', r'\[STATSCB\] statsCallback invocations: [1-9]'),
    'periodic-stats': case('periodic-stats:1000:2000'),
    'ipcthreadtrace': case('ipcthreadtrace'),
    'markers':        case('markers'),
    'no-script':      case(None),
    # The static scheduler needs a free core per thread, so it is the one
    # scheduler that notices when -n is quietly overridden: a shared config
    # include setting total_cores = 1 once turned this -n 8 run into a one-core
    # run, and pinned hid it by time-slicing.  Kept as a canary.
    'static-scheduler': case('statscb', r'\[STATSCB\] statsCallback invocations: [1-9]',
                             ['-g', '--scheduler/type=static']),

    # The hook scripts in scripts/ that nothing loaded.  Each is run with the
    # arguments its docstring documents and checked for what it says it does.

    # Marks application start and exit, and says so.
    'appevents':      case('appevents', (r'\[APP\] 0 start', r'\[APP\] 0 exit')),

    # BBV deltas per core, every 100 us (the default), to stdout ...
    'bbvtrace':       case('bbvtrace', r'\[BBV\] \d+( \d+){8}'),
    # ... or to a file in the output directory, named by the first argument.
    'bbvtrace-file':  case('bbvtrace:bbv.txt:100000',
                           check=file_has_lines('bbv.txt', 2)),

    # Core 1 to 3 GHz at 1 us, back to 2.66 GHz at 2.5 us (the docstring's
    # example).  Nothing is printed; the transition cost on core 1 is the trace.
    'dvfs':           case('dvfs:1000:1:3000:2500:1:2660', check=dvfs_took_effect),

    # Application output as markers, values 1 and 2, prefix "app".  Needs
    # sim.mem.read to work (it reads the written buffer back out of the
    # simulated address space).
    'output-as-markers': case('output-as-markers:1:2:app', check=markers_hold_output),

    # A statistics snapshot every 100k instructions.
    'periodicins-stats': case('periodicins-stats:100000', args=INS_HOOK_ARGS,
                              check=periodicins_snapshots),

    # The Python scheduler: re-places threads every 100 us by equal time.  It
    # drives thread affinity, so the pinned scheduler underneath follows it.
    'scheduler-locality': case('scheduler-locality:100000:equal_time'),

    # Lets the application start and stop the ROI through a SimUser command.
    # The smoke workload never sends one, so the only thing to check is the
    # guard the script documents: without --roi-script it refuses.  (With
    # --roi-script the run never enters the ROI, and run-sniper's own
    # post-processing then fails on the missing roi-begin snapshot.)
    'simuserroi':     case('simuserroi',
                           r'\[SimUserROI\] ERROR: --roi-script is not set'),

    # Rate of an arbitrary statistic per core, every 10 us, to stdout ...
    'stattrace':      case('stattrace:core.instructions::10000',
                           r'\[STAT:core\.instructions\] \d+( [\d.]+){8}'),
    # ... or to a file.
    'stattrace-file': case('stattrace:core.instructions:stat.txt:10000',
                           check=file_has_lines('stat.txt', 2)),

    # End the ROI once every core has run 100k instructions.  At one core: at
    # eight, the cores the four workers never reach stay at zero and the
    # target is never met, so the script would just watch the run finish.
    # The simulation is aborted at the target, so the application does not
    # get to print its checksum; the heartbeat file records the progress.
    'stop-by-icount-percore': case(
        'stop-by-icount-percore:100000',
        r'\[PERCORE-ICOUNT\] All 1 cores reached 100000 instructions\. Ending ROI\.',
        args=['-n', '1'] + INS_HOOK_ARGS, app_output=False,
        check=file_has_lines('heartbeat.txt', 3)),

    # End the ROI after 100 us of simulated time, well inside the run.  Aborts
    # the simulation, so no application checksum.
    'stop-by-time':   case('stop-by-time:100000',
                           r'\[STOPBYTIME\] Ending ROI after 100000 nanoseconds',
                           app_output=False),

    # Every system call, entry and exit.
    'syscalls':       case('syscalls', (r'\[SYSCALL\] @\s*\d+ ns: \w+\[\d+\]\s+thread\(\s*\d+\) core\(\s*\d+\) args\(',
                                        r'\[SYSCALL\] @\s*\d+ ns:\s+exit thread\(\s*\d+\) core\(\s*\d+\) ret_val\(')),

    # The thread criticality predictor: accumulates the memory CPI components
    # per core every 100 us and answers SimUtil(SIM_USER_TCP) queries.  It
    # reads the interval timer's CPI stack, so the core has to be an interval
    # core (baseline's is a ROB core); and the smoke workload never queries
    # it, so the periodic accounting running to a clean exit is the check.
    'tcp':            case('tcp', args=['-g', '--perf_model/core/type=interval']),
}


def build_app(workdir):
    exe = os.path.join(workdir, 'threaded')
    cc = os.environ.get('CC', 'gcc')
    subprocess.run([cc, '-O1', '-pthread', '-o', exe, APP_SRC], check=True)
    return exe



def run_in_own_group(cmd, timeout, **kwargs):
    """subprocess.run(), but kill the whole process group on timeout.

    subprocess.run(timeout=) kills only the direct child.  That child is
    run-sniper, a Python script, and the simulator it launches is a grandchild:
    on a timeout the simulator survived, detached, and kept running.  A suite
    run against a configuration that deadlocks therefore leaked one stuck
    simulator per timed-out case -- six of them, still resident after eleven
    hours, were what prompted this.

    start_new_session puts the child in its own process group so the whole
    tree can be signalled.
    """
    with subprocess.Popen(cmd, start_new_session=True, **kwargs) as p:
        try:
            out, _ = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                p.kill()
            p.communicate()
            raise
        return subprocess.CompletedProcess(cmd, p.returncode, out, None)

def run_case(name, exe, workdir, timeout):
    c = CASES[name]
    script = c['script']
    outdir = os.path.join(workdir, 'out-' + name)
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)

    # A case's own arguments come after SNIPER_ARGS, so its -n or -g wins.
    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper')] + SNIPER_ARGS + c['args']
    if script:
        cmd += ['-s', script]
    cmd += ['-d', outdir, '--', exe]

    env = dict(os.environ)
    # Hook scripts live in $SNIPER_ROOT/scripts; test-only fixtures live here.
    env['PYTHONPATH'] = os.pathsep.join(
        [os.path.join(HERE, 'scripts'), env.get('PYTHONPATH', '')])

    try:
        p = run_in_own_group(cmd, timeout, cwd=os.path.join(HERE, 'scripts'),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             env=env, text=True)
    except subprocess.TimeoutExpired:
        return False, 'timed out after %ds (deadlock?)' % timeout

    out = p.stdout
    if p.returncode != 0:
        detail = 'exit %d' % p.returncode
        if p.returncode == -11 or p.returncode == 139:
            detail += ' (SIGSEGV)'
        elif p.returncode == -6 or p.returncode == 134:
            detail += ' (SIGABRT)'
        return False, detail
    if c['app_output'] and not EXPECTED_APP_OUTPUT.search(out):
        return False, 'application checksum missing or wrong'
    if 'Traceback (most recent call last)' in out:
        return False, 'Python exception escaped a hook'
    for extra_re in c['expect']:
        if not re.search(extra_re, out):
            return False, 'expected output %r not seen' % extra_re
    if c['check']:
        try:
            return c['check'](outdir, out)
        except Exception as e:
            return False, 'check raised %s: %s' % (type(e).__name__, e)
    return True, 'ok'


def main():
    # Line-buffer stdout.
    #
    # Python block-buffers when stdout is a file, so a redirected run writes
    # nothing for 8KB at a time.  A sweep that takes an hour then looks
    # identical to one that has hung -- same file size, same mtime, for twenty
    # minutes at a stretch -- and the only way to tell them apart is to go
    # looking at process tables.  Progress output is worth nothing if it
    # arrives after the run.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:      # pragma: no cover - Python < 3.7
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument('--case', action='append', choices=sorted(CASES),
                    help='run only this case (repeatable)')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    add_failfast(ap)
    args = ap.parse_args()

    if not os.path.exists(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    cases = args.case or sorted(CASES)
    workdir = tempfile.mkdtemp(prefix='sniper-scripting-')
    failures = []
    log = RunLog('scripting')
    stopped_early = False
    try:
        exe = build_app(workdir)
        for name in cases:
            for it in range(args.repeat):
                label = name if args.repeat == 1 else '%s [%d/%d]' % (name, it + 1, args.repeat)
                sys.stdout.write('%-28s ' % label)
                sys.stdout.flush()
                started = time.time()
                ok, msg = run_case(name, exe, workdir, args.timeout)
                took = log.record(label, time.time() - started,
                                  'PASS' if ok else 'FAIL')
                if ok:
                    status = 'PASS' if msg == 'ok' else 'PASS (%s)' % msg
                else:
                    status = 'FAIL: ' + msg
                print('%s [%ds]' % (status, round(took)))
                if not ok:
                    failures.append((label, msg))
                    if args.failfast:
                        raise FailFast()
    except FailFast:
        stopped_early = True
    finally:
        if args.keep or stopped_early:
            if stopped_early:
                print('\nstopped at the first failure;', end=' ')
            print('work directory kept at %s' % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)
        log.finish()

    print()
    if failures:
        print('%d of %d checks FAILED' % (len(failures), len(cases) * args.repeat))
        return 1
    print('all %d checks passed' % (len(cases) * args.repeat))
    return 0


if __name__ == '__main__':
    sys.exit(main())
