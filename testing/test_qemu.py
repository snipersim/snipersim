#!/usr/bin/env python3
"""
Regression test: the QEMU frontend still produces traces Sniper can read.

The frontend is a TCG plugin loaded into a stock QEMU, so it has two sets of
things that can break underneath it: QEMU's plugin API, which is versioned and
has changed incompatibly before, and Sniper's SIFT writer.  Nothing else in the
tree exercises either.

Everything here is self-contained -- hand-written guests, no downloads, no
guest OS -- so it is cheap enough to run every time.  A booted Linux is a
better *fidelity* check and a much worse regression test: see the notes at the
bottom of frontend/qemu-plugin/README.md.

Skips cleanly, with a reason, when QEMU or the plugin is not built; only
`make QEMU=ON` produces them.

Usage:
  python3 testing/test_qemu.py
  python3 testing/test_qemu.py --case system-exact
"""

import argparse
import collections
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from common.runlog import RunLog

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
PROBE_DIR = os.path.join(SNIPER_ROOT, 'test', 'qemu-plugin-probe')

# The loop in boot.S, counted over [0x7c00, 0x7c1f): a 7-instruction prologue
# then 1000 iterations of (1 + 3*1000 + 2).  The window deliberately stops
# before the block that exits QEMU -- those instructions, and the hlt after
# them, are machinery for ending the run, not part of what is being measured.
BOOT_LOOP_INSNS = 7 + 1000 * (1 + 3 * 1000 + 2)

# boot64.S maps this virtual address to this physical one, which no identity
# mapping can produce.
VA_WATCH = 0x40000000
PA_EXPECT = 0x200000


def build_dir():
    env = os.environ.get('SNIPER_BUILD_DIR')
    for d in ([env] if env else []) + [os.path.join(SNIPER_ROOT, 'build', 'default')]:
        if d and os.path.isfile(os.path.join(d, 'config', 'sniper.py')):
            return d
    return None


def prerequisites():
    """Return (paths, None) or (None, reason-to-skip)."""
    b = build_dir()
    if not b:
        return None, 'no build directory (run make first)'
    p = {
        'build': b,
        'frontend': os.path.join(b, 'lib', 'libsniper-qemu.so'),
        'user': os.path.join(SNIPER_ROOT, 'qemu', 'build', 'qemu-x86_64'),
        'system': os.path.join(SNIPER_ROOT, 'qemu', 'build', 'qemu-system-x86_64'),
        'siftdump': os.path.join(b, 'lib', 'siftdump'),
    }
    if not os.path.isfile(p['frontend']):
        return None, 'QEMU frontend not built (make QEMU=ON)'
    if not os.path.isfile(p['user']) or not os.path.isfile(p['system']):
        return None, 'QEMU not built (deps/sniper-deps --qemu fetch)'
    return p, None


def run(cmd, timeout, **kwargs):
    """Run in its own process group so a timeout takes the whole tree with it."""
    kwargs.setdefault('stdout', subprocess.PIPE)
    kwargs.setdefault('stderr', subprocess.STDOUT)
    kwargs.setdefault('text', True)
    with subprocess.Popen(cmd, start_new_session=True, **kwargs) as proc:
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                proc.kill()
            out, _ = proc.communicate()
            return None, out or ''
        return proc.returncode, out or ''


def build_guests(workdir):
    """Build the probe and its hand-written guests against the pinned QEMU."""
    qemu_src = os.path.join(SNIPER_ROOT, 'qemu')
    rc, out = run(['make', '-s', 'QEMU_SRC=' + qemu_src], 300, cwd=PROBE_DIR)
    if rc != 0:
        raise RuntimeError('cannot build the probe:\n' + out)
    return {n: os.path.join(PROBE_DIR, n)
            for n in ('probe.so', 'boot.img', 'boot64.img', 'st', 'mt')}


# --- cases ------------------------------------------------------------------

def case_user_trace(p, g, workdir):
    """The frontend writes a SIFT trace of a single-threaded guest."""
    prefix = os.path.join(workdir, 'user')
    rc, out = run([p['user'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', g['st']], 300, cwd=workdir)
    if rc != 0:
        return False, 'qemu-x86_64 exited %s' % rc
    if 'st done' not in out:
        return False, 'guest produced no output'
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    if not m:
        return False, 'frontend reported no instruction count'
    n = int(m.group(1))
    # st is a fixed arithmetic loop; the count is stable to the startup path.
    if not (30e6 < n < 35e6):
        return False, '%d instructions, expected ~32M' % n
    trace = prefix + '.app0.th0.sift'
    if not os.path.isfile(trace) or os.path.getsize(trace) < 1000:
        return False, 'no usable trace at %s' % os.path.basename(trace)
    rc, _ = run([p['siftdump'], trace], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (exit %s)' % rc
    return True, '%d instructions, trace readable' % n


def case_user_simulate(p, g, workdir):
    """Sniper simulates a trace the frontend produced."""
    prefix = os.path.join(workdir, 'sim')
    rc, out = run([p['user'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', os.path.join(PROBE_DIR, 'st')], 300, cwd=workdir)
    if rc != 0:
        return False, 'recording failed (exit %s)' % rc
    outdir = os.path.join(workdir, 'simout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'),
                   '-n', '1', '-c', 'nehalem-lite', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        return False, 'run-sniper exited %d' % rc
    m = re.search(r'Simulated ([\d.]+)M instructions, ([\d.]+)M cycles, ([\d.]+) IPC', out)
    if not m:
        return False, 'no simulation summary'
    ipc = float(m.group(3))
    if not (0.1 < ipc < 4.0):
        return False, 'implausible IPC %s' % ipc
    return True, '%sM instructions, IPC %s' % (m.group(1), ipc)


def case_user_mt_streams(p, g, workdir):
    """A multi-threaded guest gets one SIFT stream per thread.

    This is the mode that works without flow control: the plugin never blocks,
    so the hazard that makes *blocking* multi-threaded user mode unsafe cannot
    arise, and Sniper gets one trace per thread.

    The refusal path for blocking mode is not exercised here.  It needs a
    Sniper on the other end of the response channel -- standalone, the writer
    aborts opening the channel before a second thread ever appears -- so it
    waits on run-sniper learning to drive QEMU.  The hazard it guards against
    is reproduced in test/qemu-plugin-probe.
    """
    prefix = os.path.join(workdir, 'mt')
    rc, out = run([p['user'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', g['mt'], '3', '5'], 300, cwd=workdir)
    if rc is None:
        return False, 'hung'
    if rc != 0:
        return False, 'qemu-x86_64 exited %d' % rc
    if 'mt done' not in out:
        return False, 'guest produced no output'
    traces = sorted(f for f in os.listdir(workdir)
                    if f.startswith('mt.app0.th') and f.endswith('.sift'))
    if len(traces) < 4:          # main + 3 spinners, at least
        return False, 'only %d trace(s): %s' % (len(traces), ', '.join(traces))
    for t in traces:
        rc, _ = run([p['siftdump'], os.path.join(workdir, t)], 300)
        if rc != 0:
            return False, '%s unreadable (exit %s)' % (t, rc)
    return True, '%d per-thread traces, all readable' % len(traces)


def case_system_exact(p, g, workdir):
    """Full-system instruction counting matches a hand-computed total."""
    rc, out = run([p['system'], '-nographic', '-accel', 'tcg', '-m', '64',
                   '-drive', 'file=%s,format=raw,if=floppy' % g['boot.img'],
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                   '-plugin', '%s,quantum=100000,lo=0x7c00,hi=0x7c1f' % g['probe.so'],
                   '-no-reboot'], 120, cwd=workdir)
    if rc is None:
        return False, 'timed out'
    m = re.search(r'range_insns=(\d+)', out)
    if not m:
        return False, 'probe reported no count (exit %s)' % rc
    n = int(m.group(1))
    if n != BOOT_LOOP_INSNS:
        return False, 'counted %d, expected exactly %d' % (n, BOOT_LOOP_INSNS)
    return True, 'exactly %d instructions' % n


def case_system_vapa(p, g, workdir):
    """Paged VA -> PA agrees with the page tables the guest built."""
    rc, out = run([p['system'], '-nographic', '-accel', 'tcg', '-m', '128',
                   '-drive', 'file=%s,format=raw,if=floppy' % g['boot64.img'],
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                   '-plugin', '%s,watch=0x%x' % (g['probe.so'], VA_WATCH),
                   '-no-reboot'], 120, cwd=workdir)
    if rc is None:
        return False, 'timed out'
    hits = re.findall(r'WATCH va=0x%x hwaddr_pa=0x([0-9a-f]+) translate_vaddr=0x([0-9a-f]+)'
                      % VA_WATCH, out)
    if not hits:
        return False, 'the watched address was never seen'
    for hw, tr in hits:
        if int(hw, 16) != PA_EXPECT or int(tr, 16) != PA_EXPECT:
            return False, 'va 0x%x -> hwaddr 0x%s / translate 0x%s, expected 0x%x' % (
                VA_WATCH, hw, tr, PA_EXPECT)
    return True, '%d accesses, both APIs give 0x%x' % (len(hits), PA_EXPECT)


def case_system_modes(p, g, workdir):
    """The frontend tracks x86 mode switches through a real BIOS boot.

    The boot floppy runs in 16-bit real mode, but it only gets there after
    SeaBIOS, which switches to 32-bit protected mode and back hundreds of times
    to run its own 32-bit code.  Getting this wrong is not a cosmetic problem:
    the mode selects the XED decode tables, so a misidentified block is decoded
    against the wrong instruction set and either aborts the simulator or feeds
    the timing model instructions the guest never executed.

    The assertions below are about structure rather than exact counts, because
    the counts move with the SeaBIOS build:

      - the very first mode seen is 16-bit, at the reset vector 0xfffffff0;
      - no block is ever called 64-bit, because SeaBIOS never enters long mode;
      - the switches concentrate at a handful of addresses.  SeaBIOS changes
        mode in one pair of routines, so hundreds of switches share two static
        addresses.  Many distinct addresses would mean the detector is flapping
        on ambiguous encodings instead of following the machine.
    """
    out_base = os.path.join(workdir, 'modes')
    rc, out = run([p['system'], '-nographic', '-display', 'none', '-no-reboot',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                   '-drive', 'file=%s,format=raw,if=floppy' % g['boot.img'],
                   '-plugin', '%s,verbose=true,output=%s' % (p['frontend'], out_base)],
                  300, cwd=workdir)
    if rc is None:
        return False, 'timed out'

    sw = re.findall(r'now in (\d+)-bit mode at 0x([0-9a-f]+)', out)
    if not sw:
        return False, 'no mode switches reported (exit %s)' % rc

    if sw[0] != ('16', 'fffffff0'):
        return False, 'first mode is %s-bit at 0x%s, expected 16-bit at the ' \
                      'reset vector 0xfffffff0' % sw[0]

    if any(bits == '64' for bits, _ in sw):
        return False, 'reported 64-bit mode; SeaBIOS never enters long mode'

    addrs = collections.Counter(a for _, a in sw)
    top2 = sum(n for _, n in addrs.most_common(2))
    if len(sw) > 20 and top2 < 0.8 * len(sw):
        return False, 'switches scattered over %d addresses (top two are only ' \
                      '%d of %d) -- detector is flapping' % (len(addrs), top2, len(sw))

    trace = out_base + '.app0.th0.sift'
    if not os.path.isfile(trace):
        return False, 'no trace written'

    return True, '%d switches, %d-bit first, %d of %d at two addresses' % (
        len(sw), int(sw[0][0]), top2, len(sw))


CASES = {
    'user-trace':       case_user_trace,
    'user-simulate':    case_user_simulate,
    'user-mt-streams':  case_user_mt_streams,
    'system-exact':     case_system_exact,
    'system-vapa':      case_system_vapa,
    'system-modes':     case_system_modes,
}


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
    ap.add_argument('--case', action='append', choices=sorted(CASES))
    args = ap.parse_args()

    p, why = prerequisites()
    if why:
        print('SKIP: %s' % why)
        return 0

    workdir = tempfile.mkdtemp(prefix='sniper-qemu-')
    failures = []
    log = RunLog('qemu')
    try:
        g = build_guests(workdir)
        for name in (args.case or sorted(CASES)):
            started = time.time()
            ok, detail = CASES[name](p, g, workdir)
            took = log.record(name, time.time() - started,
                              'PASS' if ok else 'FAIL')
            print('%-20s %s [%ds]'
                  % (name, 'PASS (%s)' % detail if ok else 'FAIL: ' + detail,
                     round(took)))
            if not ok:
                failures.append(name)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
        run(['make', '-s', 'clean'], 60, cwd=PROBE_DIR)
        log.finish()

    print()
    total = len(args.case or CASES)
    if failures:
        print('%d of %d checks FAILED' % (len(failures), total))
        return 1
    print('all %d checks passed' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
