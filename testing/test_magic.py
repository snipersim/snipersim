#!/usr/bin/env python3
"""
Regression test: the magic-instruction API (include/sim_api.h) works the same
on every architecture and through every front end that can run it.

One freestanding program, test/qemu-plugin-probe/magic-api.c, built for
x86-64, AArch64 and RISC-V, calls each standard command and prints what came
back.  It is run:

  - natively (x86-64) or under QEMU without the front end: no simulator to
    answer, so every command must come back as its own number -- a program
    carrying magic instructions behaves on hardware as if they were absent;
  - through each front end that reaches its architecture, under run-sniper
    with two cores and magic-api.py:

        x86-64    SDE, DynamoRIO, QEMU
        AArch64   QEMU
        RISC-V    QEMU

    where each answer is held against what the simulator was set up to say:
    SimInSimulator() 1, core 0, thread 0, 2 cores, 1 thread, the
    configuration's clock, then 1234 MHz after SimSetFreqMHz(0, 1234),
    SimUser(0x5a5a, 41) answered 42 by the script, the instruction count and
    simulated time moving on across some work.  SimMarker and SimNamedMarker
    must reach the script with their arguments (the name read out of guest
    memory), and SimSetThreadName must reach the statistics database.

The magic instruction per architecture: x86 "xchg %bx, %bx"; AArch64
"bfm x0, x0, #0, #0"; RISC-V "slti zero, zero, 0x5a" (a HINT, which the ISA
designates for custom use: a no-op on hardware).

Needs clang (SNIPER_RISCV_CC or SNIPER_AARCH64_CC) for the AArch64 and RISC-V
builds; those rows are skipped, with the reason, without it.

Usage:
  python3 testing/test_magic.py [--only x86_64-qemu ...]
"""

import argparse
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from common.failfast import add_argument as add_failfast
from common.runlog import RunLog

PROBE_DIR = os.path.join(SNIPER_ROOT, 'test', 'qemu-plugin-probe')
SOURCE = os.path.join(PROBE_DIR, 'magic-api.c')
SCRIPT = os.path.join(PROBE_DIR, 'magic-api.py')
WORK = 7358464                 # magic-api.c's checksum of its work, the same everywhere

QEMU_USER = {
    'x86_64':  ('SNIPER_QEMU_X86_64',  os.path.join(SNIPER_ROOT, 'qemu', 'build', 'qemu-x86_64')),
    'aarch64': ('SNIPER_QEMU_AARCH64', os.path.join(SNIPER_ROOT, 'qemu-aarch64', 'build', 'qemu-aarch64')),
    'riscv64': ('SNIPER_QEMU_RISCV',   os.path.join(SNIPER_ROOT, 'qemu-riscv', 'build', 'qemu-riscv64')),
}
CONFIG = {'x86_64': 'gainestown', 'aarch64': 'cortex-a72', 'riscv64': 'riscv'}

# (architecture, front end): the run-sniper options that select it
CASES = {
    'x86_64-sde':     ('x86_64', []),
    'x86_64-dr':      ('x86_64', ['--frontend=dr']),
    'x86_64-qemu':    ('x86_64', ['--frontend=qemu']),
    'aarch64-qemu':   ('aarch64', ['--frontend=qemu']),
    'riscv64-qemu':   ('riscv64', ['--frontend=qemu']),
    'native':         (None, None),
}


def run(cmd, timeout, **kw):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           timeout=timeout, start_new_session=True, **kw)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        return None, (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')


def build_dir():
    return os.environ.get('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))


def qemu_for(arch):
    var, path = QEMU_USER[arch]
    path = os.environ.get(var) or path
    return path if os.access(path, os.X_OK) else None


def build(arch, workdir):
    """(binary, None) or (None, why)."""
    clang = os.environ.get('SNIPER_AARCH64_CC' if arch == 'aarch64' else 'SNIPER_RISCV_CC') \
            or os.environ.get('SNIPER_RISCV_CC')
    if arch == 'x86_64' and not (clang and os.path.isfile(clang)):
        cmd = ['cc']
    elif clang and os.path.isfile(clang):
        cmd = [clang, '--target=%s-linux-gnu' % arch, '-fuse-ld=lld']
        if arch == 'riscv64':
            cmd += ['-march=rv64gc', '-mabi=lp64d']
    else:
        return None, 'no clang for %s (set SNIPER_RISCV_CC or SNIPER_AARCH64_CC)' % arch
    out = os.path.join(workdir, 'magic-api-' + arch)
    rc, o = run(cmd + ['-O1', '-ffreestanding', '-nostdlib', '-static', '-Wl,-e,_start',
                       '-I', os.path.join(SNIPER_ROOT, 'include'), '-o', out, SOURCE], 120)
    if rc != 0:
        return None, 'could not build magic-api.c for %s: %s' % (arch, o.strip()[-200:])
    return out, None


def answers(out):
    return {k: int(v) for k, v in re.findall(r'^(?:.*\] )?api ([a-z_]+)=(\d+)', out, re.M)}


def check_native(workdir):
    """No simulator: every command comes back as its own number."""
    want = {'in_simulator': 0, 'proc_id': 9, 'thread_id': 10, 'num_procs': 11, 'num_threads': 12,
            'mhz_initial': 7, 'mhz_after_set': 7, 'user': 5, 'ins_delta': 0, 'time_grew': 0, 'work': WORK}
    done = []
    for arch in ('x86_64', 'aarch64', 'riscv64'):
        binary, why = build(arch, workdir)
        if not binary:
            continue
        cmd = [binary] if arch == 'x86_64' else [qemu_for(arch), binary] if qemu_for(arch) else None
        if not cmd:
            continue
        rc, out = run(cmd, 120)
        got = answers(out)
        if rc != 0 or got != want:
            return False, '%s: exit %s, answered %s, expected %s' % (arch, rc, got, want)
        done.append(arch)
    if not done:
        return None, 'nothing to run'
    return True, 'every command came back unchanged on %s' % ', '.join(done)


def unavailable(arch, frontend):
    b = build_dir()
    if frontend and '--frontend=qemu' in frontend:
        if not os.path.isfile(os.path.join(b, 'lib', 'libsniper-qemu.so')):
            return 'no QEMU frontend in %s (make QEMU=ON)' % b
        if not qemu_for(arch):
            return 'no QEMU for %s' % arch
    if frontend and '--frontend=dr' in frontend:
        if not os.path.isfile(os.path.join(b, 'lib', 'libdr-frontend.so')):
            return 'no DynamoRIO frontend in %s (make DYNAMORIO=ON)' % b
    return None


def check_simulated(arch, frontend, workdir, timeout):
    binary, why = build(arch, workdir)
    if not binary:
        return None, why
    outdir = os.path.join(workdir, 'sim-%s-%s' % (arch, (frontend or ['sde'])[0].split('=')[-1]))
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '2', '-c', CONFIG[arch], '-d', outdir,
                   '-s', SCRIPT] + frontend + ['--', binary], timeout, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'timed out after %ds (a magic instruction left unanswered?)' % timeout
    if rc != 0:
        if 'built without the armv8 decoder' in out:
            return None, 'Sniper built without the ARM decoder (make ARM=ON)'
        return False, 'run-sniper exited %s' % rc
    got = answers(out)
    want = {'in_simulator': 1, 'proc_id': 0, 'thread_id': 0, 'num_procs': 2, 'num_threads': 1,
            'mhz_after_set': 1234, 'user': 42, 'time_grew': 1, 'work': WORK}
    bad = ['%s=%s (want %s)' % (k, got.get(k), v) for k, v in want.items() if got.get(k) != v]
    # the configuration's own clock, before the change
    m = re.search(r'^\s*frequency\s*=\s*([\d.]+)', open(os.path.join(outdir, 'simulation', 'sim.cfg')).read(), re.M)
    clock = int(round(float(m.group(1)) * 1000)) if m else None
    if got.get('mhz_initial') != clock:
        bad.append('mhz_initial=%s (want the configured %s)' % (got.get('mhz_initial'), clock))
    # four rounds of 4096 iterations: tens of instructions each, on any ISA
    if not 4 * 4096 * 4 <= got.get('ins_delta', 0) <= 4 * 4096 * 40:
        bad.append('ins_delta=%s (want the work\'s instructions)' % got.get('ins_delta'))
    markers = re.findall(r'\[MAGIC-API\] marker thread=(\d+) a=(\d+) b=(\d+) str=(\S*)', out)
    if markers != [('0', '7', '8', ''), ('0', '9', '0', 'api-marker')]:
        bad.append('markers %s (want 7/8, then 9 named api-marker)' % markers)
    try:
        db = sqlite3.connect(os.path.join(outdir, 'simulation', 'sim.stats.sqlite3'))
        names = [r[0] for r in db.execute('select description from event where event = 2')]   # EVENT_THREAD_NAME
    except sqlite3.Error as e:
        names = ['(%s)' % e]
    if names != ['api-worker']:
        bad.append('thread names %s (want api-worker)' % names)
    if bad:
        return False, '; '.join(bad)
    return True, 'all answers right (%s MHz, then 1234; %d instructions of work); markers and thread name arrived' % (
        clock, got['ins_delta'])


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append', choices=sorted(CASES))
    ap.add_argument('--timeout', type=int, default=600)
    add_failfast(ap)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(build_dir(), 'lib', 'sniper')):
        print('error: %s/lib/sniper not built; run make first' % build_dir(), file=sys.stderr)
        return 2

    log = RunLog('magic')
    workdir = tempfile.mkdtemp(prefix='sniper-magic-')
    failures = skipped = 0
    names = args.only or sorted(CASES)
    for name in names:
        arch, frontend = CASES[name]
        print('%-14s ' % name, end='')
        t0 = time.time()
        if arch is None:
            ok, detail = check_native(workdir)
        else:
            why = unavailable(arch, frontend)
            ok, detail = (None, why) if why else check_simulated(arch, frontend, workdir, args.timeout)
        took = time.time() - t0
        if ok is None:
            print('SKIP (%s)' % detail)
            skipped += 1
            continue
        log.record(name, took, 'PASS' if ok else 'FAIL')
        print('%s (%s) [%ds]' % ('PASS' if ok else 'FAIL', detail, round(took)))
        if not ok:
            failures += 1
            if args.failfast:
                break
    log.finish()
    print()
    if failures:
        print('%d of %d checks FAILED (%d skipped)' % (failures, len(names), skipped))
        print('working directory kept: %s' % workdir)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print('all %d checks passed (%d skipped)' % (len(names) - skipped, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
