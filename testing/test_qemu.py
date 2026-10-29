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
import glob
import collections
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
PROBE_DIR = os.path.join(SNIPER_ROOT, 'test', 'qemu-plugin-probe')
# The AArch64 guest's CPU: every extension QEMU has, with SVE registers of the
# 16 bytes the cortex configurations model (perf_model/core/arm/sve_bytes), as
# record-trace gives it.  QEMU's own default is 64; the simulator refuses a
# recording whose width differs from the configuration's.
A64_CPU = 'max,sve-default-vector-length=16'

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

def exit_text(rc):
    """How run() ended, for a failure message: a timeout comes back as None."""
    return 'timed out' if rc is None else 'exit %s' % rc

def sniper_failed(rc, out):
    """A run-sniper failure with the end of its output, which is where the
    simulator's backtrace (a crash), its assertion or the error it stopped on
    is; a one-off failure under the whole suite is otherwise unexplained
    once the temporary directory is gone."""
    tail = [l.strip()[:160] for l in out.splitlines() if l.strip()][-10:]
    return 'run-sniper %s%s' % (exit_text(rc), (': ' + ' | '.join(tail)) if tail else '')


def build_guests(workdir):
    """Build the probe and its hand-written guests against the pinned QEMU, in
    a copy of test/qemu-plugin-probe under this run's directory: the probe
    directory is shared by every run of this suite, and two suites at once --
    or one suite's `make clean` at its end -- left the other without its boot
    images and probe (every x86 system-mode case failed within a second)."""
    qemu_src = os.path.join(SNIPER_ROOT, 'qemu')
    probe = os.path.join(workdir, 'probe')
    os.makedirs(probe, exist_ok=True)
    for f in os.listdir(PROBE_DIR):
        if f.endswith(('.c', '.S', '.h', '.py')) or f == 'Makefile':
            shutil.copy(os.path.join(PROBE_DIR, f), probe)
    rc, out = run(['make', '-s', 'QEMU_SRC=' + qemu_src], 300, cwd=probe)
    if rc != 0:
        raise RuntimeError('cannot build the probe:\n' + out)
    return {n: os.path.join(probe, n)
            for n in ('probe.so', 'boot.img', 'boot64.img', 'bootas.img', 'bootsmc.img', 'st', 'mt')}


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
        return False, 'siftdump could not read the trace (%s)' % exit_text(rc)
    return True, '%d instructions, trace readable' % n


def case_user_simulate(p, g, workdir):
    """Sniper simulates a trace the frontend produced."""
    prefix = os.path.join(workdir, 'sim')
    rc, out = run([p['user'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', g['st']], 300, cwd=workdir)
    if rc != 0:
        return False, 'recording failed (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'simout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'),
                   '-n', '1', '-c', 'nehalem-lite', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        return False, sniper_failed(rc, out)
    m = re.search(r'Simulated ([\d.]+)M instructions, ([\d.]+)M cycles, ([\d.]+) IPC', out)
    if not m:
        return False, 'no simulation summary'
    ipc = float(m.group(3))
    if not (0.1 < ipc < 4.0):
        return False, 'implausible IPC %s' % ipc
    return True, '%sM instructions, IPC %s' % (m.group(1), ipc)


def case_user_fork_refused(p, g, workdir):
    """A guest that forks is refused with the reason (the plugin records one
    process; a child would write into the parent's streams): recorded to
    files, the child stops at its first step with the .refused marker and the
    parent runs on; live, run-sniper fails the run and names the reason."""
    guest = os.path.join(workdir, 'fork_child')
    rc, out = run(['gcc', '-O1', '-static', '-o', guest, os.path.join(SNIPER_ROOT, 'testing', 'apps', 'fork_child.c')], 120)
    if rc != 0:
        return None, 'could not build fork_child.c statically (%s)' % exit_text(rc)
    reason = '[SNIPER-QEMU] error: the guest created a process'
    prefix = os.path.join(workdir, 'fork')
    rc, out = run([p['user'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    marker = prefix + '.app0.refused'
    if not os.path.isfile(marker) or reason not in open(marker).read():
        return False, 'recorded to files: no refusal marker for the child (qemu exit %s): %s' % (exit_text(rc), out[-300:])
    if 'fork_child parent sum=' not in out or 'fork_child child sum=' in out:
        return False, 'recorded to files: the parent should run on and the child stop: %s' % out[-300:]
    outdir = os.path.join(workdir, 'fork-live')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-d', outdir,
                   '--frontend=qemu', '--', guest], 600, cwd=SNIPER_ROOT)
    if rc != 1 or ('The front end refused the run: ' + reason) not in out:
        return False, 'live: expected the run to fail with the refusal, got %s: %s' % (exit_text(rc), sniper_failed(rc, out))
    return True, 'the forked child refused with the reason, recorded and live (run exit 1)'


def case_user_mt_streams(p, g, workdir):
    """A multi-threaded guest gets one SIFT stream per thread.

    This is the mode that works without flow control: the plugin never blocks,
    so the hazard that makes *blocking* multi-threaded user mode unsafe cannot
    arise, and Sniper gets one trace per thread.

    The live run (flow control, a response channel) is user-mt-live's; the
    hazard it needs the plugin wait call for is reproduced in
    test/qemu-plugin-probe.
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
            return False, '%s unreadable (%s)' % (t, exit_text(rc))
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
        return False, 'probe reported no count (%s)' % exit_text(rc)
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


def case_system_address_spaces(p, g, workdir):
    """Two processes' code and data at one virtual address stay apart.

    The guest (bootas.S) maps VA 0x40000000 to two physical pages, with
    different code and a store to the same virtual address in each, and
    switches CR3 between them four times.  QEMU translates code by physical
    page, so it re-translates neither process's code after the first time:
    a trace keyed by virtual address carried the second process's bytes for
    every later run of the first.  The trace now carries each page's
    physical address (Writer::Mapping, CodePhysical), so the code at that
    address must alternate A, B, A, B, the stores must reach two physical
    pages, and the whole trace -- BIOS, real mode, the switch into long mode
    -- must simulate."""
    prefix = os.path.join(workdir, 'as')
    rc, out = run([p['system'], '-nographic', '-display', 'none', '-accel', 'tcg', '-m', '128',
                   '-drive', 'file=%s,format=raw,if=floppy' % g['bootas.img'],
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-no-reboot',
                   '-plugin', '%s,output=%s' % (p['frontend'], prefix)], 300, cwd=workdir)
    if rc is None:
        return False, 'timed out'
    if rc != 33:          # isa-debug-exit: (0x10 << 1) | 1, the guest's own checks passed
        return False, 'the guest failed its own checks (%s)' % exit_text(rc)
    trace = prefix + '.app0.th0.sift'
    rc, dump = run([p['siftdump'], trace], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (%s)' % exit_text(rc)
    code = [l.split()[1] for l in dump.splitlines() if l.split() and l.split()[0].lstrip('0') == '40000000']
    if code != ['b8', '31'] * 4:
        return False, 'first bytes at 0x40000000: %s, expected b8 and 31 alternately, 4 times each' % ' '.join(code)
    stores = re.findall(r'-- addr 40000800=([0-9a-f]+)', dump)
    if stores != ['200800', '400800'] * 4:
        return False, 'stores to 0x40000800 went to %s, expected 200800 and 400800 alternately' % ' '.join(stores)
    outdir = os.path.join(workdir, 'assim')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'gainestown', '-d', outdir,
                   '--traces=' + trace], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'simulating the full-system trace failed (%s)' % exit_text(rc)
    m = re.search(r'Simulated ([\d.]+)M instructions', out)
    return True, 'code A/B alternate at one address, stores at two physical pages; simulated %sM instructions' % (
        m.group(1) if m else '?')


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
        return False, 'no mode switches reported (%s)' % exit_text(rc)

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


# --- RISC-V, user mode --------------------------------------------------
#
# The same plugin, loaded into a RISC-V QEMU: it reads the target from QEMU at
# install and classifies branches from the encoding.  Needs a plugin-capable
# qemu-riscv64 (SNIPER_QEMU_RISCV) and, to build the guest, a RISC-V compiler
# (SNIPER_RISCV_CC, a clang with lld or a riscv64 gcc); the simulation side
# needs the RISC-V core model (make RISCV=ON, the default).  Skips,
# with the reason, when any of those is missing.

def riscv_toolchain():
    """The qemu-riscv64 and the clang the RISC-V cases use: SNIPER_QEMU_RISCV
    and SNIPER_RISCV_CC when set, else the tree's own QEMU (make QEMU=ON) and a
    clang on the PATH that has the riscv64 target, as test_magic and
    test_pacsim default them.  Without the defaults the RISC-V cases skipped,
    silently, on a tree that had everything they needed."""
    qemu = os.environ.get('SNIPER_QEMU_RISCV') or os.path.join(SNIPER_ROOT, 'qemu-riscv', 'build', 'qemu-riscv64')
    cc = os.environ.get('SNIPER_RISCV_CC')
    if not cc:
        for name in ('clang-15', 'clang-16', 'clang-17', 'clang-18', 'clang-19', 'clang-20', 'clang'):
            found = shutil.which(name)
            if found:
                cc = found
                break
    if cc and os.path.isfile(cc) and 'SNIPER_RISCV_CC' not in os.environ:
        os.environ['SNIPER_RISCV_CC'] = cc      # the cases read it from the environment
    return qemu, cc


def riscv_prerequisites(workdir):
    qemu, cc = riscv_toolchain()
    if not qemu or not os.path.isfile(qemu):
        return None, 'no qemu-riscv64 built with --enable-plugins (make QEMU=ON, or set SNIPER_QEMU_RISCV)'
    if not cc or not os.path.isfile(cc):
        return None, 'no clang with the riscv64 target (set SNIPER_RISCV_CC)'
    if 'SNIPER_QEMU_RISCV' not in os.environ:
        os.environ['SNIPER_QEMU_RISCV'] = qemu
    guest = os.path.join(workdir, 'rv-loop')
    if not os.path.isfile(guest):
        rc, out = run([cc, '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d', '-O1',
                       '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest,
                       os.path.join(PROBE_DIR, 'rv-loop.c')], 120)
        if rc != 0:
            return None, 'could not build rv-loop.c (%s)' % exit_text(rc)
    return {'qemu': qemu, 'guest': guest}, None


def riscv_system_guest(workdir, name, sources, flags=()):
    """A bare-metal guest for QEMU's riscv64 virt board, or (None, why)."""
    qemu = os.environ.get('SNIPER_QEMU_RISCV_SYSTEM') or \
           os.path.join(SNIPER_ROOT, 'qemu-riscv-system', 'build', 'qemu-system-riscv64')
    cc = os.environ.get('SNIPER_RISCV_CC')
    if not os.path.isfile(qemu):
        return None, 'no qemu-system-riscv64 (make RISCV_IME=ON, or set SNIPER_QEMU_RISCV_SYSTEM)'
    if not cc or not os.path.isfile(cc):
        return None, 'SNIPER_RISCV_CC not set to a RISC-V compiler'
    guest = os.path.join(workdir, name)
    # -bios none starts the hart at 0x80000000, so the start code goes there
    rc, out = run([cc, '--target=riscv64-unknown-elf', '-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany',
                   '-O1', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,--section-start=.boot=0x80000000',
                   '-Wl,-Ttext=0x80001000', '-Wl,-e,_start', '-o', guest] + list(flags) +
                  [os.path.join(PROBE_DIR, s) for s in sources], 120)
    if rc != 0:
        return None, 'could not build %s (%s)' % (name, exit_text(rc))
    return {'qemu': qemu, 'guest': guest}, None


def run_riscv_system(p, r, prefix, workdir):
    return run([r['qemu'], '-M', 'virt', '-bios', 'none', '-nographic', '-kernel', r['guest'],
                '-plugin', '%s,output=%s' % (p['frontend'], prefix)], 300, cwd=workdir)


def case_riscv_system_simulate(p, g, workdir):
    """RISC-V full system: a bare-metal guest on the virt board is traced from
    the reset vector, with physical addresses, and simulates instruction for
    instruction."""
    r, why = riscv_system_guest(workdir, 'rv-bare', ['rv-bare.c'])
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvsys')
    rc, out = run_riscv_system(p, r, prefix, workdir)
    if rc != 0:
        return False, 'qemu-system-riscv64 exited %s (the guest\'s own check failed, or it crashed)' % rc
    if 'rv sum=3490074579968' not in out:
        return False, 'the guest computed the wrong answer'
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    written = int(m.group(1)) if m else 0
    outdir = os.path.join(workdir, 'rvsys-sim')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'simulating the trace failed (%s)' % exit_text(rc)
    stats = {}
    for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
        if ' = ' in line:
            k, v = line.split(' = ', 1)
            stats[k.strip()] = v.strip().split(',')[0]
    n = int(stats.get('core.instructions', 0))
    if not written or abs(n - written) > written // 1000:
        return False, 'simulated %d instructions of the %d traced' % (n, written)
    return True, '%d instructions from the reset vector, traced and simulated' % written


def case_riscv_system_address_spaces(p, g, workdir):
    """RISC-V full system: two Sv39 address spaces with different code and data
    at one virtual address stay apart in the trace (rv-as.c; bootas.S is the
    x86 version)."""
    r, why = riscv_system_guest(workdir, 'rv-as', ['rv-as.c', 'rv-as-code.S'])
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvas')
    rc, out = run_riscv_system(p, r, prefix, workdir)
    if rc != 0 or 'rv-as ok' not in out:
        return False, 'the guest failed its own checks (%s)' % exit_text(rc)
    rc, dump = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (%s)' % exit_text(rc)
    code = [l.split()[1] for l in dump.splitlines() if l.split() and l.split()[0].lstrip('0') == '40000000']
    if code != ['37', '09'] * 4:
        return False, 'first bytes at 0x40000000: %s, expected 37 (lui) and 09 (c.li) alternately' % ' '.join(code)
    stores = re.findall(r'-- addr 400007f0=([0-9a-f]+)', dump)
    if stores != ['802007f0', '804007f0'] * 4:
        return False, 'stores to 0x400007f0 went to %s, expected 802007f0 and 804007f0 alternately' % ' '.join(stores)
    outdir = os.path.join(workdir, 'rvas-sim')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'simulating the trace failed (%s)' % exit_text(rc)
    return True, 'code A/B alternate at one address, stores at two physical pages; simulated'


def case_aarch64_user_simulate(p, g, workdir):
    """ARM64 without ARM hardware: QEMU AArch64 user mode records, Sniper simulates.

    Two guests, built with the same clang as the RISC-V ones: a64-loop.c, and
    a64-stress.c -- scalar FP, auto-vectorised NEON, divides, load/store pairs,
    a jump table, indirect calls and atomics, built for ARMv8.2 so the atomics
    are LSE (ldadd, cas), which the Capstone pinned before 5.0 could not
    decode.  Each is traced by the front end in qemu-aarch64 and simulated on
    an out-of-order and an in-order ARM core (cortex-a72, cortex-a53); the
    instruction counts must agree with the trace, and each guest's own answer
    must be right."""
    qemu = os.environ.get('SNIPER_QEMU_AARCH64') or os.path.join(SNIPER_ROOT, 'qemu-aarch64', 'build', 'qemu-aarch64')
    cc = os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')
    if not os.path.isfile(qemu):
        return None, 'no qemu-aarch64 (make ARM=ON QEMU=ON, or set SNIPER_QEMU_AARCH64)'
    if not cc or not os.path.isfile(cc):
        return None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    guests = [('a64-loop', ['-O1'], 'a64 sum=3490074579968'),
              ('a64-stress', ['-O3', '-fno-math-errno', '-march=armv8.2-a+lse+crc+fp16'],
               'a64 stress=13765554095865117910')]
    summary = []
    for name, flags, answer in guests:
        guest = os.path.join(workdir, name)
        rc, out = run([cc, '--target=aarch64-linux-gnu'] + flags + ['-ffreestanding', '-nostdlib', '-static',
                       '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, os.path.join(PROBE_DIR, name + '.c')], 120)
        if rc != 0:
            return None, 'could not build %s.c (%s)' % (name, exit_text(rc))
        prefix = os.path.join(workdir, name)
        rc, out = run([qemu, '-cpu', A64_CPU, '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest],
                      300, cwd=workdir)
        if rc != 0:
            return False, '%s: qemu-aarch64 exited %s' % (name, rc)
        if answer not in out:
            return False, '%s computed the wrong answer' % name
        m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
        if not m:
            return False, '%s: the front end reported no instruction count' % name
        written = int(m.group(1))
        ipcs = []
        for cfg in ('cortex-a72', 'cortex-a53'):
            outdir = os.path.join(workdir, '%s-%s' % (name, cfg))
            shutil.rmtree(outdir, ignore_errors=True)
            rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', cfg, '-d', outdir,
                           '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
            if rc != 0:
                if 'built without the armv8 decoder' in out:
                    return None, 'Sniper built without the ARM decoder (make ARM=ON)'
                return False, '%s on %s: %s' % (name, cfg, sniper_failed(rc, out))
            stats = {}
            for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
                if ' = ' in line:
                    k, v = line.split(' = ', 1)
                    stats[k.strip()] = v.strip().split(',')[0]
            n = int(stats.get('core.instructions', 0))
            if abs(n - written) > written // 1000:
                return False, '%s on %s simulated %d instructions of the %d traced' % (name, cfg, n, written)
            m = re.search(r'Simulated [\d.]+M instructions, [\d.]+M cycles, ([\d.]+) IPC', out)
            ipcs.append('%s %s' % (cfg.split('-')[1], m.group(1) if m else '?'))
        summary.append('%s: %d instructions, IPC %s' % (name, written, ', '.join(ipcs)))
    return True, '; '.join(summary)


def case_riscv_smc_rewrite(p, g, workdir):
    """Code rewritten in place is traced with its new bytes, and simulates.

    The guest runs code A, rewrites the same address with compressed code B,
    then with A again (rv-smc-rewrite.c).  The plugin reports each
    re-translation to the trace writer (Sift::CodeTracker), which sends the
    page again; before that, the trace carried A's bytes for all three runs.
    smc=0 turns the tracking off and must give the old, stale trace."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'rv-smc-rewrite')
    if not os.path.isfile(guest):
        rc, out = run([os.environ['SNIPER_RISCV_CC'], '--target=riscv64-linux-gnu', '-march=rv64gc',
                       '-mabi=lp64d', '-O1', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start',
                       '-o', guest, os.path.join(PROBE_DIR, 'rv-smc-rewrite.c')], 120)
        if rc != 0:
            return False, 'could not build rv-smc-rewrite.c (%s)' % exit_text(rc)
    seen = {}
    for smc in ('1', '0'):
        prefix = os.path.join(workdir, 'smc%s' % smc)
        rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-plugin', '%s,output=%s,smc=%s' % (p['frontend'], prefix, smc),
                       '--', guest], 300, cwd=workdir)
        if rc != 0:
            return False, 'qemu-riscv64 exited %s (smc=%s)' % (rc, smc)
        m = re.search(r'B rewritten pc=0x([0-9a-f]+) returned=2', out)
        if not m:
            return False, 'the guest did not run code B correctly (smc=%s)' % smc
        pc = m.group(1)
        rc, dump = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
        if rc != 0:
            return False, 'siftdump could not read the trace (%s)' % exit_text(rc)
        seen[smc] = [l.split()[1:3] for l in dump.splitlines()
                     if l.split() and l.split()[0].lstrip('0') == pc.lstrip('0')]
    want = [['13', '05'], ['09', '45'], ['13', '05']]
    if seen['1'] != want:
        return False, 'traced %s at the rewritten address, expected %s' % (seen['1'], want)
    if seen['0'] == want:
        return False, 'smc=0 traced the new bytes too: the option has no effect'
    outdir = os.path.join(workdir, 'smcsim')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + os.path.join(workdir, 'smc1') + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'simulating the trace failed (%s)' % exit_text(rc)
    return True, 'traced A, B, A at one address (%s); smc=0 gives the stale %s; simulated' % (
        ', '.join(' '.join(x) for x in seen['1']), ', '.join(' '.join(x) for x in seen['0']))


def case_riscv_user_trace(p, g, workdir):
    """The frontend traces a RISC-V guest: the exact count, and the trace reads back."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rv')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['guest']], 300, cwd=workdir)
    if rc != 100:        # the guest exits with work(20000) & 0x7f == 100
        return False, 'qemu-riscv64 exited %s' % rc
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    if not m:
        return False, 'frontend reported no instruction count'
    n = int(m.group(1))
    if n != 100007:
        return False, '%d instructions, expected 100007' % n + versions_note(r['qemu'])
    rc, _ = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (%s)' % exit_text(rc)
    return True, '%d instructions, trace readable' % n


def case_riscv_vector_width(p, g, workdir):
    """The guest's vector width against the configuration's: the plugin reports
    vlenb (SIM_CMD_VECTOR_BYTES) and the simulator refuses a run that times
    the vector unit at another width -- they used to match only by
    convention.  A guest recorded at 256 bits is refused by the riscv
    configuration (16 bytes) with the reason, and runs with vector_bytes=32."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'vw256')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', IME_CPU_DEFAULT.replace('vlen=128', 'vlen=256'),
                   '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', r['guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    trace = '--traces=' + prefix + '.app0.th0.sift'
    outdir = os.path.join(workdir, 'vw-wrong')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir, trace], 600, cwd=SNIPER_ROOT)
    if rc == 0 or "The guest's vector registers are 32 bytes" not in out:
        return False, 'a 256-bit recording on a 128-bit configuration was not refused (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'vw-right')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-g', 'perf_model/core/riscv/vector_bytes=32',
                   '-d', outdir, trace], 600, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'with vector_bytes=32 the run failed: %s' % sniper_failed(rc, out)
    return True, 'a 256-bit guest refused at 16 bytes with the reason, simulated at 32'


def case_riscv_power(p, g, workdir):
    """run-sniper --power on a RISC-V guest: McPAT's estimate for the riscv
    configuration, a non-x86 core at its own process node.  It used to die in
    mcpat.py (no bus statistics: the configuration's network keeps none) and
    the run passed with no power files; and every core was described to McPAT
    as x86."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    outdir = os.path.join(workdir, 'rvpower')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '--power', '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--frontend=qemu', '--', r['guest']], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'the run failed: %s' % sniper_failed(rc, out)
    txt, xml = os.path.join(outdir, 'power.txt'), os.path.join(outdir, 'power.xml')
    if not os.path.isfile(txt) or not os.path.isfile(xml):
        return False, 'no power.txt / power.xml'
    t = open(txt).read()
    m = re.search(r'^Processor:.*?Runtime Dynamic = ([0-9.]+) W', t, re.S | re.M)
    if not m or float(m.group(1)) <= 0:
        return False, 'power.txt has no processor runtime power'
    if 'Technology 22 nm' not in t:
        return False, 'McPAT did not use the configuration\'s 22 nm'
    if 'name="x86" value="0"' not in open(xml).read():
        return False, 'power.xml describes the RISC-V core as x86'
    return True, 'McPAT: %s W runtime dynamic at 22 nm, a non-x86 core' % m.group(1)


def case_riscv_user_simulate(p, g, workdir):
    """Sniper's RISC-V core model simulates the trace; branches agree with the decoder."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvsim')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'rvsimout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        if 'Unknown core model riscv' in out:
            return None, 'Sniper built without the RISC-V core model (make RISCV=ON)'
        return False, sniper_failed(rc, out)
    stats = {}
    for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
        if ' = ' in line:
            k, v = line.split(' = ', 1)
            stats[k.strip()] = v.strip().split(',')[0]
    n = int(stats.get('performance_model.instruction_count', 0))
    correct = int(stats.get('branch_predictor.num-correct', 0))
    wrong = int(stats.get('branch_predictor.num-incorrect', 0))
    # Every retired instruction is simulated: the ROB is drained when the
    # stream ends, and the last instruction is handed over with a synthesized
    # fall-through successor.
    if n != 100007:
        return False, '%d instructions simulated, expected exactly 100007' % n + versions_note()
    if correct + wrong != 20000:
        return False, '%d branches predicted, expected the loop\'s 20000' % (correct + wrong)
    # The second BOOM configuration (MediumBoom) runs the same trace: the
    # same instructions, a narrower core, so no fewer cycles.
    outdir2 = os.path.join(workdir, 'rvsimout-mediumboom')
    shutil.rmtree(outdir2, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv-mediumboom', '-d', outdir2,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'riscv-mediumboom: %s' % sniper_failed(rc, out)
    st2 = stats_of(outdir2)
    n2 = int(st2.get('performance_model.instruction_count', 0))
    if n2 != n:
        return False, 'riscv-mediumboom simulated %d instructions, riscv %d' % (n2, n)
    cyc, cyc2 = int(float(stats.get('performance_model.cycle_count', 0))), int(float(st2.get('performance_model.cycle_count', 0)))
    if cyc2 < cyc:
        return False, 'riscv-mediumboom took %d cycles, fewer than the wider riscv core\'s %d' % (cyc2, cyc)
    return True, '%d instructions, %d of %d branches predicted; riscv %d cycles, riscv-mediumboom %d' % (n, correct, correct + wrong, cyc, cyc2)


# The IME (Zvvm 0.9) cases need a qemu-riscv64 built with the extension's CPU
# properties; SNIPER_QEMU_RISCV_IME_CPU overrides the -cpu string.  The guest
# is built like rv-loop.c.  Skips when the QEMU lacks the properties.
IME_CPU_DEFAULT = 'max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on'
# Exact counts below -- rv-loop's 100,007 instructions, rv-ime's 33, the
# translation guest's 517 faults and 12 or 14 table frames, the gdbstub's 103
# registers -- are properties of the guest binary and the QEMU that runs it:
# measured with these, and another compiler or QEMU can move them.  A check
# that misses one says which it ran with (versions_note), so a toolchain
# change reads as one, not as a regression.
MEASURED_WITH = {'clang': '15.0.6', 'qemu': '11.1.1'}
_versions = {}

def versions_note(qemu=None):
    """' (measured with clang A and QEMU B; this run: clang C, QEMU D)'."""
    cc = os.environ.get('SNIPER_RISCV_CC') or os.environ.get('SNIPER_AARCH64_CC') or ''
    qemu = qemu or os.environ.get('SNIPER_QEMU_RISCV') or os.path.join(SNIPER_ROOT, 'qemu-riscv', 'build', 'qemu-riscv64')
    for key, exe in (('clang', cc), ('qemu', qemu)):
        if (key, exe) not in _versions:
            try:
                out = subprocess.run([exe, '--version'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, timeout=30).stdout
                m = re.search(r'version (\d+\.\d+\.\d+)', out)
                _versions[(key, exe)] = m.group(1) if m else 'unknown'
            except (OSError, subprocess.SubprocessError):
                _versions[(key, exe)] = 'unknown'
    now = {'clang': _versions[('clang', cc)], 'qemu': _versions[('qemu', qemu)]}
    return ' (measured with clang %s and QEMU %s; this run: clang %s, QEMU %s%s)' % (
        MEASURED_WITH['clang'], MEASURED_WITH['qemu'], now['clang'], now['qemu'],
        '' if now == MEASURED_WITH else ' -- another toolchain, which can move the count')

IME_INSNS = 33            # rv-ime.c: exact instruction count of the guest (built on include/riscv_insn.h)
IME_FOOTPRINTS = 4        # three tile loads and one tile store
IME_SPANS = 8             # each tile has two rows in different cache lines
IME_SPAN_BYTES = 64       # 8 rows of 8 bytes

def riscv_ime_prerequisites(workdir):
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    cc = os.environ['SNIPER_RISCV_CC']
    guest = os.path.join(workdir, 'rv-ime')
    if not os.path.isfile(guest):
        # rv64gcv: the standard vector configuration instruction is native; the
        # matrix instructions come from the generated include/riscv_insn.h
        rc, out = run([cc, '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O1',
                       '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start',
                       '-I', os.path.join(SNIPER_ROOT, 'include'), '-o', guest,
                       os.path.join(PROBE_DIR, 'rv-ime.c')], 120)
        if rc != 0:
            return None, 'could not build rv-ime.c (%s)' % exit_text(rc)
    cpu = os.environ.get('SNIPER_QEMU_RISCV_IME_CPU', IME_CPU_DEFAULT)
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', cpu, guest], 60, cwd=workdir)
    if rc != 100:
        if 'zvvm' in out or 'Property' in out:
            return None, 'qemu-riscv64 has no IME (x-zvvmm) CPU properties'
        if rc in (-4, 132):      # SIGILL: this QEMU has no IME; that is a skip
            return None, 'the IME guest took SIGILL under qemu-riscv64: no IME in this QEMU'
        return False, 'the IME guest exited %s under qemu-riscv64 (expected 100)' % exit_text(rc)
    r['cpu'] = cpu
    r['ime_guest'] = guest
    return r, None

def case_riscv_ime_trace(p, g, workdir):
    """The frontend records every byte range a tile access touches, ahead of the instruction."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvime')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['ime_guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'qemu-riscv64 exited %s' % rc
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    n = int(m.group(1)) if m else -1
    if n != IME_INSNS:
        return False, '%d instructions, expected %d' % (n, IME_INSNS) + versions_note()
    m = re.search(r'\[SNIPER-QEMU\] (\d+) memory footprints recorded', out)
    f = int(m.group(1)) if m else 0
    if f != IME_FOOTPRINTS:
        return False, '%d memory footprints recorded, expected %d' % (f, IME_FOOTPRINTS)
    rc, out = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (%s)' % exit_text(rc)
    spans = re.findall(r'[0-9a-f]{8}\+(\d+)([rw])', out)
    if len(spans) != IME_SPANS or any(int(b) != 8 for b, _ in spans):
        return False, 'siftdump shows %d spans %s, expected %d of 8 bytes' % (len(spans), spans, IME_SPANS)
    if sum(1 for _, d in spans if d == 'w') != 2:
        return False, 'expected the tile store to contribute two written spans, got %s' % spans
    return True, '%d instructions, %d footprints, %d spans (2 written)' % (n, f, len(spans))

def case_riscv_ime_simulate(p, g, workdir):
    """Sniper decodes the IME instructions, counts them, and the cache sees every tile row."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvimesim')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['ime_guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'rvimesimout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        if 'Unknown core model riscv' in out:
            return None, 'Sniper built without the RISC-V core model (make RISCV=ON)'
        return False, sniper_failed(rc, out)
    if 'undecoded instruction' in out:
        return False, 'the decoder did not recognise an instruction: %s' % re.search(r'undecoded instruction[^\n]*', out).group(0)
    stats = {}
    for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
        if ' = ' in line:
            k, v = line.split(' = ', 1)
            stats[k.strip()] = v.strip().split(',')[0]
    want = {'performance_model.instruction_count': IME_INSNS,
            'riscv.ime_tile_load': 3, 'riscv.ime_tile_store': 1, 'riscv.ime_mma_int': 1,
            'riscv.ime_cfg': 1, 'riscv.v_cfg': 1,
            'riscv.spans': IME_SPANS, 'riscv.span_bytes': IME_SPAN_BYTES,
            'L1-D.stores-data': 2}
    for k, v in want.items():
        got = int(stats.get(k, -1))
        if got != v:
            return False, '%s = %d, expected %d' % (k, got, v)
    return True, ('%d instructions; 3 tile loads, 1 tile store, 1 vmmacc decoded; '
                  '%d spans reached the cache, 2 of them stores') % (IME_INSNS, IME_SPANS)

# gemm3: one matrix product three ways (scalar, RVV 1.0, IME 0.9), self-
# verifying (exit 100 when all agree), traced and simulated with the
# instruction-class and footprint counts checked against the values
# tools/riscv/expected_counts.py derives from the specification, and, when a
# sail-riscv model is at hand (SNIPER_SAIL_RISCV, or the sail-riscv
# dependency), the scalar-versus-vector comparison run on that golden model
# at VLEN=128 (the model has no IME; the IME path is left out there).
GEMM_DIR = os.path.join(SNIPER_ROOT, 'test', 'riscv-gemm')

def gemm_build(cc, out, extra):
    return run([cc, '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O1', '-ffreestanding',
                '-nostdlib', '-static', '-fuse-ld=lld', '-I', os.path.join(SNIPER_ROOT, 'include')] + extra
               + ['-o', out, os.path.join(GEMM_DIR, 'gemm3.c')], 120)

def riscv_gemm_prerequisites(workdir):
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gemm3')
    if not os.path.isfile(guest):
        rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest, ['-Wl,-e,_start'])
        if rc != 0:
            return None, 'could not build gemm3.c (%s)' % exit_text(rc)
    r['gemm'] = guest
    return r, None

def case_riscv_gemm_verify(p, g, workdir):
    """Scalar, RVV and IME compute the same product: the guest says so with exit 100."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], r['gemm']], 120, cwd=workdir)
    names = {100: 'all three agree', 5: 'RVV differs from scalar', 6: 'IME differs from scalar', 7: 'RVV and IME differ from scalar'}
    if rc != 100:
        return False, 'guest exited %s: %s' % (rc, names.get(rc, 'unexpected'))
    return True, 'scalar, RVV 1.0 and IME 0.9 results agree (8x8x8 int32)'

def case_riscv_gemm_simulate(p, g, workdir):
    """Sniper's per-class instruction counts and footprint bytes equal the specification's."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'gemm3sim')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['gemm']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'gemm3simout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        return False, sniper_failed(rc, out)
    if 'undecoded instruction' in out:
        return False, re.search(r'undecoded instruction[^\n]*', out).group(0)
    rc, out = run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'riscv', 'expected_counts.py'),
                   os.path.join(outdir, 'simulation', 'sim.stats')], 60)
    if rc != 0:
        bad = [l for l in out.splitlines() if l.startswith('FAIL')]
        return False, 'counts off: ' + '; '.join(bad)[:300]
    last = [l for l in out.splitlines() if l.startswith('compute:')]
    return True, 'every class count and the footprint bytes match the specification (%s)' % (last[0] if last else '')

def case_riscv_gemm_sail(p, g, workdir):
    """The scalar and RVV 1.0 paths agree on the sail-riscv golden model at VLEN=128."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    sail = os.environ.get('SNIPER_SAIL_RISCV') or os.path.join(SNIPER_ROOT, 'sail-riscv', 'bin', 'sail_riscv_sim')
    if not os.path.isfile(sail):
        return None, 'no sail-riscv model (SNIPER_SAIL_RISCV, or deps/sniper-deps --riscv-ime)'
    guest = os.path.join(workdir, 'gemm3-sail')
    rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest,
                         ['-DPLATFORM_HTIF', '-DNO_IME', '-Wl,-T,' + os.path.join(GEMM_DIR, 'sail.ld')])
    if rc != 0:
        return False, 'could not build the HTIF variant (%s)' % exit_text(rc)
    override = os.path.join(workdir, 'sail-v128.json')
    with open(override, 'w') as f:
        f.write('{ "extensions": { "V": { "vlen_exp": 7 } } }\n')
    rc, out = run([sail, '--config-override', override, guest], 600, cwd=workdir)
    if 'SUCCESS' not in out:
        tail = out.strip().splitlines()[-1] if out.strip() else 'no output'
        return False, 'sail-riscv did not report SUCCESS: %s' % tail[:200]
    return True, 'sail-riscv 0.14 (RVV 1.0, VLEN=128) runs the scalar and vector paths to agreement'

# The LLM agent workload (test/llm-agent): a tiny GPT trained on the host
# calls tools over an agent protocol, freestanding on RISC-V, verifying
# every generated line against the numpy model's own greedy output and the
# answers against the tools.  Four kernel variants (scalar, RVV, and the
# weight matvecs on IME 0.9 tiles or the AME draft's squares) must agree;
# the simulation case reports the batch-of-1 cost in cycles per token for
# each accelerated variant and checks the matrix-unit instruction counts
# against what the model's shape implies.
LLM_DIR = os.path.join(SNIPER_ROOT, 'test', 'llm-agent')
LLM_VARIANTS = (('scalar', []), ('rvv', ['-DUSE_RVV']), ('ime', ['-DUSE_IME']), ('ame', ['-DUSE_AME']), ('dma', ['-DUSE_DMA']), ('w4', ['-DUSE_W4']))

def llm_build(cc, out, extra):
    return run([cc, '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O2', '-ffreestanding',
                '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-I', LLM_DIR,
                '-I', os.path.join(SNIPER_ROOT, 'include')] + extra
               + ['-o', out, os.path.join(LLM_DIR, 'agent.c')], 300)

def llm_dims(keys='DLV'):
    d = {}
    for line in open(os.path.join(LLM_DIR, 'model_weights.h')):
        m = re.match(r'#define GPT_([DLVH]) (\d+)', line)
        if m:
            d[m.group(1)] = int(m.group(2))
    return tuple(d[k] for k in keys)

def llm_vector_counts(vlen, prefilled, generated, batches, att_dots, att_strips):
    """The riscv.v_<class> counts the RVV build implies.  Its vector work is
    three strip loops over e32,m4 groups of VL = VLEN/8 elements: the dot
    product (every matvec row, the head, and att_dots q.k products of HD
    elements, a count the guest reports because it depends on the positions),
    layer normalisation (three passes over D) and GELU (one pass over 4D);
    attention adds, per (token, layer, head) call over n_t = pos + 1 scores, a
    max reduction and an exp pass in strips (att_strips, reported by the guest)
    and one multiply-accumulate per value row into an HD-wide accumulator.
    The per-strip instruction counts are those of the loops in agent.c."""
    D, L, V, H = llm_dims('DLVH')
    VL = vlen // 8
    S = lambda n: (n + VL - 1) // VL
    tokens, heads = prefilled + generated, generated + len(batches)
    dots = {D: tokens * L * (3 * D + D + 4 * D) + heads * V, 4 * D: tokens * L * D, D // H: att_dots}
    ln, gelu = tokens * 2 * L + heads, tokens * L
    e = {k: 0 for k in ('v_cfg', 'v_load', 'v_store', 'v_fp_muladd', 'v_fp_addsub', 'v_fp_div', 'v_int_alu')}
    for n, calls in dots.items():
        e['v_cfg'] += calls * (S(n) + 2); e['v_load'] += calls * 2 * S(n); e['v_fp_muladd'] += calls * S(n)
        e['v_fp_addsub'] += calls * 2; e['v_int_alu'] += calls * 2
    s = S(D)
    e['v_cfg'] += ln * (3 * s + 4); e['v_load'] += ln * 5 * s; e['v_store'] += ln * s
    e['v_fp_addsub'] += ln * (4 * s + 4); e['v_fp_muladd'] += ln * 3 * s; e['v_int_alu'] += ln * 4
    s = S(4 * D)
    e['v_cfg'] += gelu * s; e['v_load'] += gelu * s; e['v_store'] += gelu * s
    e['v_fp_muladd'] += gelu * 15 * s; e['v_fp_addsub'] += gelu * 13 * s; e['v_fp_div'] += gelu * s; e['v_int_alu'] += gelu * 3 * s
    calls, sh = tokens * L * H, S(D // H)                    # attention: one call per (token, layer, head)
    e['v_cfg'] += (att_strips + 2 * calls) + (att_strips + 2 * calls) + calls * sh
    e['v_load'] += att_strips + att_strips + att_dots * sh
    e['v_store'] += att_strips + calls * sh
    e['v_fp_addsub'] += (att_strips + 3 * calls) + (12 * att_strips + 2 * calls)
    e['v_fp_muladd'] += 9 * att_strips + (calls + att_dots) * sh
    e['v_int_alu'] += (2 * att_strips + 2 * calls) + calls * sh
    return {'riscv.' + k: v for k, v in e.items()}

# Unit geometry per kernel variant: the matrix side S (an IME tile or an AME
# square is S x S) and the accumulators the GEMM blocks over (IME: 16/LMUL
# register groups, AME: 16 accumulators).
# Every riscv64 user-mode run here is laid out as record-trace lays it out: QEMU
# gives the guest 48 bits by default, the riscv configurations model Sv39, and
# a guest above 2^38 is refused by the simulator (see 3_level_radix_sv39.cfg).
RISCV_VA_RESERVE = ['-R', '0x4000000000']

LLM_GEOMETRY = {'ime': (2, 16), 'ime32': (32, 2), 'ame': (4, 16), 'ame32': (32, 16)}

# The memory side of the agent runs: an edge LPDDR bandwidth (config/riscv.cfg
# ships 5 GB/s), and the DRAM traffic per token in the summaries.
LLM_DRAM_GBS = 25.6   # the riscv configuration's per_controller_bandwidth, for the report text
LLM_DRAM_ARGS = []    # no override: the cases run on the configuration's default

def dram_per_token(stats, tokens):
    """DRAM traffic per token; a read or write is one 64-byte line."""
    reads, writes = int(stats.get('dram.reads', 0)), int(stats.get('dram.writes', 0))
    return 'DRAM %.0f reads + %.0f writes per token (%.2f MB)' % (reads / tokens, writes / tokens, (reads + writes) * 64 / tokens / 1e6)

def llm_matrix_counts(variant, decode_tokens, batches):
    """The riscv.<class> counts the agent's kernels imply.  Per decode token
    (and per prefill batch for the head, whose logits only the last token
    needs): matvecs per layer qkv (D->3D), out (D->D), fc1 (D->4D), fc2
    (4D->D), then the head (D->V); a matvec is one S-wide block of output
    rows at a time over S-wide K slices, the leftover rows through the RVV
    dot product.  Per prefill batch of T tokens each layer matvec is a GEMM
    over ceil(T/S) padded token blocks: a weight tile is loaded once per
    group of NACC token blocks, an activation tile once per weight tile."""
    D, L, V = llm_dims()
    layer_shapes = [(D, 3 * D), (D, D), (D, 4 * D), (4 * D, D)] * L
    e = {}
    if variant in LLM_GEOMETRY:
        S, NACC = LLM_GEOMETRY[variant]
        cfg = loads = stores = mma = 0
        def matvec(nin, nout):
            nonlocal cfg, loads, stores, mma
            blocks, ks = nout // S, nin // S
            cfg += blocks if variant.startswith('ame') else 1
            loads += blocks * ks * 2; stores += blocks; mma += blocks * ks
        for _ in range(decode_tokens):
            for nin, nout in layer_shapes + [(D, V)]:
                matvec(nin, nout)
        for T in batches:
            matvec(D, V)
            TB = (T + S - 1) // S
            groups = (TB + NACC - 1) // NACC
            for nin, nout in layer_shapes:
                blocks, ks = nout // S, nin // S
                cfg += TB * blocks if variant.startswith('ame') else 1
                loads += groups * blocks * ks + TB * blocks * ks
                mma += TB * blocks * ks
                stores += TB * blocks
        if variant.startswith('ime'):
            e = {'riscv.ime_cfg': cfg, 'riscv.ime_tile_load': loads, 'riscv.ime_tile_store': stores, 'riscv.ime_mma_fp': mma}
        else:
            e = {'riscv.ame_cfg': cfg + 3, 'riscv.ame_load': loads, 'riscv.ame_store': stores, 'riscv.ame_mma': mma,
                 'riscv.ame_move': stores, 'riscv.ame_own': 2}
    elif variant == 'dma':
        # one task per weight product: per decode token the 4L layer matvecs
        # and the head, per prefill batch the 4L GEMMs and the head matvec
        tasks = (decode_tokens + len(batches)) * (4 * L + 1)
        e = {'riscv.acc_task': tasks, 'riscv.acc_wait': tasks}
        # the accelerator's work: every layer product for every token, the head once per decode token and once per batch
        per_layer = D * 3 * D + D * D + D * 4 * D + 4 * D * D
        e['ame_tile.task_macs'] = (decode_tokens + sum(batches)) * L * per_layer + (decode_tokens + len(batches)) * D * V
    return e

def riscv_llm_prerequisites(workdir):
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    if not os.path.isfile(os.path.join(LLM_DIR, 'model_weights.h')):
        return None, 'no model_weights.h (run tools/llm/train_agent_gpt.py on the host)'
    r['llm'] = {}
    for variant, flags in LLM_VARIANTS:
        guest = os.path.join(workdir, 'agent-' + variant)
        if not os.path.isfile(guest):
            rc, out = llm_build(os.environ['SNIPER_RISCV_CC'], guest, flags)
            if rc != 0:
                return None, 'could not build agent.c (%s, exit %s)' % (variant, rc)
        r['llm'][variant] = guest
    return r, None

def case_riscv_llm_agent(p, g, workdir):
    """The tool-calling GPT answers every prompt as the numpy model does, with scalar, RVV, IME, AME, DMA-task and int4 kernels."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    summary = []
    matched = {}
    for variant, guest in r['llm'].items():
        cmd = [r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu']]
        if variant in ('ame', 'dma'):     # QEMU has neither AME nor the accelerator: the plugin executes them
            cmd += ['-plugin', '%s,output=%s' % (p['frontend'], os.path.join(workdir, 'llm' + variant)), '--']
        rc, out = run(cmd + [guest], 1800, cwd=workdir)     # the IME build under QEMU's tile helpers takes ~7 minutes alone, more on a loaded machine
        names = {5: 'a line differs from the numpy model', 6: 'an answer is wrong', 7: 'lines differ and answers are wrong'}
        if rc != 100:
            return False, '%s kernel: guest exited %s: %s' % (variant, rc, names.get(rc, 'unexpected'))
        if '[AME] error' in out:
            return False, 'the AME backend model reported: ' + [l for l in out.splitlines() if l.startswith('[AME] error')][0]
        m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', out)
        summary.append('%s: %s/%s tokens' % (variant, m.group(1), m.group(2)) if m else variant)
        # Every variant's own lines, not the last variant's: each prompt is
        # reported as matching numpy and answered correctly.
        matched[variant] = len(re.findall(r'\[= numpy\] \[correct\]', out))
    counts = set(matched.values())
    if len(counts) != 1 or 0 in counts:
        return False, 'prompts matching numpy and answered correctly differ by kernel: %s' % (
            ', '.join('%s %d' % kv for kv in sorted(matched.items())))
    return True, '%d prompts on each of %d kernels, every line as numpy, every answer right (%s)' % (
        counts.pop(), len(matched), '; '.join(summary))

def case_riscv_llm_simulate(p, g, workdir):
    """Sniper simulates the agent on RVV, IME, AME and int4 weights and reports the batch-of-1 cost per token; matrix-unit and int4 matvec counts must be exact."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    report = []
    for variant in ('rvv', 'ime', 'ame', 'w4'):
        prefix = os.path.join(workdir, 'llmsim-' + variant)
        rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                       '--', r['llm'][variant]], 1800, cwd=workdir)   # QEMU runs the IME/AME helpers for ~70M guest instructions
        if rc != 100:
            return False, '%s: recording failed (%s)' % (variant, exit_text(rc))
        m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', out)
        if not m:
            return False, '%s: no token count in the guest output' % variant
        prefilled, generated = int(m.group(1)), int(m.group(2))
        mb = re.search(r'prefill batches:((?: \d+)+)', out)
        if not mb:
            return False, '%s: no prefill batch sizes in the guest output' % variant
        batches = [int(t) for t in mb.group(1).split()]
        att = re.search(r'attention: (\d+) dots, (\d+) softmax strips', out)      # the guest's output, before the simulator's replaces it
        if variant == 'w4':
            # every layer product of every token runs as one int4 matvec (the head stays fp32)
            D, L, V = llm_dims()
            mw = re.search(r'w4: (\d+) int4 matvecs', out)
            want = (prefilled + generated) * 4 * L
            if not mw or int(mw.group(1)) != want:
                return False, 'w4: %s int4 matvecs, the model shape implies %d' % (mw.group(1) if mw else 'no', want)
        outdir = os.path.join(workdir, 'llmsimout-' + variant)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir] + LLM_DRAM_ARGS
                      + ['--traces=' + prefix + '.app0.th0.sift'], 5400, cwd=SNIPER_ROOT)
        if rc is None:
            return False, '%s: simulation timed out' % variant
        if rc != 0:
            return False, '%s: %s' % (variant, sniper_failed(rc, out))
        if 'undecoded instruction' in out:
            return False, re.search(r'undecoded instruction[^\n]*', out).group(0)
        stats = {}
        for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
            if ' = ' in line:
                k, v = line.split(' = ', 1)
                stats[k.strip()] = v.strip().split(',')[0]
        insns = int(stats.get('performance_model.instruction_count', 0))
        cycles = int(float(stats.get('performance_model.cycle_count', 0)))
        tokens = prefilled + generated
        if not cycles or int(stats.get('riscv.v_load', 0)) == 0:
            return False, '%s: no cycles or no vector loads counted' % variant
        for k, v in sorted(llm_matrix_counts(variant, generated, batches).items()):
            if k.startswith('ame_tile.'):
                continue
            got = int(stats.get(k, 0))
            if got != v:
                return False, '%s: %s = %d, the model shape implies %d' % (variant, k, got, v)
        if variant == 'rvv':
            if not att:
                return False, 'rvv: no attention dot count in the guest output'
            vlen = int(re.search(r'vlen=(\d+)', r['cpu']).group(1))
            for k, v in sorted(llm_vector_counts(vlen, prefilled, generated, batches, int(att.group(1)), int(att.group(2))).items()):
                got = int(stats.get(k, 0))
                if got != v:
                    return False, 'rvv: %s = %d, the kernels imply %d' % (k, got, v)
        report.append('%s %.0f cycles/token (%.0f insn/token, IPC %.2f; %s)' % (variant, cycles / tokens, insns / tokens, insns / cycles, dram_per_token(stats, tokens)))
    return True, '%d tokens (%d prefill + %d decode) at %s GB/s: %s; RVV vector-class, IME and AME instruction counts and the int4 matvec count exact' % (tokens, prefilled, generated, LLM_DRAM_GBS, '; '.join(report))

# AME (Attached Matrix Extension draft, Ztt): the gemm3 AME path runs on the
# backend model inside the frontend plugin (QEMU has no AME; without the
# plugin the guest dies with SIGILL), then Sniper counts it by class.
def case_riscv_ame_verify(p, g, workdir):
    """Scalar, RVV, IME and AME agree; AME executed by the plugin's backend model."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gemm3-ame')
    rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest, ['-Wl,-e,_start', '-DUSE_AME'])
    if rc != 0:
        return False, 'could not build the AME variant (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'gemm3ame')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    names = {5: 'RVV differs', 6: 'IME differs', 8: 'AME differs from scalar', 12: 'RVV and AME differ', 14: 'IME and AME differ'}
    if rc != 100:
        errs = [l for l in out.splitlines() if l.startswith('[AME] error')]
        return False, 'guest exited %s: %s%s' % (rc, names.get(rc, 'unexpected'), ('; ' + errs[0]) if errs else '')
    if '[AME] error' in out:
        return False, 'the backend model reported an error: ' + [l for l in out.splitlines() if l.startswith('[AME] error')][0]
    m = re.search(r'\[SNIPER-QEMU\] (\d+) memory footprints recorded', out)
    r['ame_trace'] = prefix + '.app0.th0.sift'
    return True, 'scalar, RVV 1.0, IME 0.9 and AME (4x4 Int32 squares) agree; %s footprints recorded' % (m.group(1) if m else '?')

def case_riscv_ame_simulate(p, g, workdir):
    """Sniper counts the AME instructions by class and their strided-square footprints exactly."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gemm3-ame')
    if not os.path.isfile(guest):
        rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest, ['-Wl,-e,_start', '-DUSE_AME'])
        if rc != 0:
            return False, 'could not build the AME variant (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'gemm3amesim')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'gemm3amesimout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None or rc != 0:
        return False, sniper_failed(rc, out)
    if 'undecoded instruction' in out:
        return False, re.search(r'undecoded instruction[^\n]*', out).group(0)
    rc, out = run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'riscv', 'expected_counts.py'), '--ame',
                   os.path.join(outdir, 'simulation', 'sim.stats')], 60)
    if rc != 0:
        return False, 'counts off: ' + '; '.join(l for l in out.splitlines() if l.startswith('FAIL'))[:300]
    return True, 'every class count (2 own, 7 cfg, 16 square loads, 4 stores, 8 mmulbtacc.2d, 4 moves) and the footprint bytes match'

def case_riscv_llm_couplings(p, g, workdir):
    """The agent on the attached tile (AME) and on the DMA-attached accelerator under the L2 and near-core couplings: counts exact, cycles per token reported."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    report = []
    for variant in ('ame', 'dma'):
        prefix = os.path.join(workdir, 'llmcpl-' + variant)
        rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                       '--', r['llm'][variant]], 1800, cwd=workdir)   # QEMU runs the IME/AME helpers for ~70M guest instructions
        if rc != 100:
            return False, '%s: recording failed (%s)' % (variant, exit_text(rc))
        m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', out)
        mb = re.search(r'prefill batches:((?: \d+)+)', out)
        if not m or not mb:
            return False, '%s: no token or batch counts in the guest output' % variant
        prefilled, generated = int(m.group(1)), int(m.group(2))
        batches = [int(t) for t in mb.group(1).split()]
        tokens = prefilled + generated
        for coupling in ('oca_l2', 'nca'):
            outdir = os.path.join(workdir, 'llmcpl-%s-%s' % (variant, coupling))
            shutil.rmtree(outdir, ignore_errors=True)
            rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                           '-g', 'perf_model/ame_tile/coupling=' + coupling] + LLM_DRAM_ARGS
                          + ['--traces=' + prefix + '.app0.th0.sift'], 5400, cwd=SNIPER_ROOT)
            if rc is None or rc != 0:
                return False, '%s/%s: %s' % (variant, coupling, sniper_failed(rc, out))
            stats = {}
            for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
                if ' = ' in line:
                    k, v = line.split(' = ', 1)
                    stats[k.strip()] = v.strip().split(',')[0]
            cycles = int(float(stats.get('performance_model.cycle_count', 0)))
            insns = int(stats.get('performance_model.instruction_count', 0))
            want = llm_matrix_counts(variant, generated, batches)
            if variant == 'dma':
                want['ame_tile.tasks'] = want['riscv.acc_task']
            else:
                want.pop('ame_tile.task_macs', None)
            for k, v in sorted(want.items()):
                if int(stats.get(k, -1)) != v:
                    return False, '%s/%s: %s = %s, the model shape implies %d' % (variant, coupling, k, stats.get(k), v)
            if variant == 'ame' and int(stats.get('ame_tile.mma', 0)) != want['riscv.ame_mma']:
                return False, '%s/%s: the tile executed %s squares, the core counted %d' % (variant, coupling, stats.get('ame_tile.mma'), want['riscv.ame_mma'])
            report.append('%s/%s %.0f cycles/token (%.0f insn/token; %s)' % (variant, coupling, cycles / tokens, insns / tokens, dram_per_token(stats, tokens)))
    return True, '%d tokens at %s GB/s: %s; tile and task counts exact' % (tokens, LLM_DRAM_GBS, '; '.join(report))

# The units at their wide geometry: 32x32 fp32 IME tiles (VLEN=4096, e32, m8,
# lambda 4: the QEMU patch raises RV_VLEN_MAX to 4096) and 32x32 AME squares
# (AME_NELEM = 1024, the plugin model's ame_n=32).  A square product of side
# 32 is 32768 multiply-accumulates: an in-core issue interval of 32 cycles
# (a 32x32 array) and vector operands of 512 bytes.
SIZE32_CPU = IME_CPU_DEFAULT.replace('vlen=128', 'vlen=4096')
SIZE32_IME_FLAGS = ['-DUSE_IME', '-DIME_VLEN=4096', '-DIME_LMUL=8', '-DIME_LAMBDA=4']
SIZE32_AME_FLAGS = ['-DUSE_AME', '-DAME_N=32']

def case_riscv_size32_verify(p, g, workdir):
    """gemm3 at 64x64x64 with 32x32 IME tiles and 32x32 AME squares: all four paths agree and every count is exact."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gemm3-32')
    rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest, ['-Wl,-e,_start'] + SIZE32_AME_FLAGS + SIZE32_IME_FLAGS + ['-DM=64', '-DN=64', '-DK=64'])
    if rc != 0:
        return False, 'could not build the 32x32 variant (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'gemm3-32')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', SIZE32_CPU, '-plugin', '%s,output=%s,ame_n=32' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    if rc != 100:
        if 'VLEN in the range' in out:
            return None, 'this qemu-riscv64 caps VLEN below 4096 (rebuild with the current IME patch)'
        return False, 'guest exited %s (100 = scalar, RVV, IME and AME agree)' % rc
    outdir = os.path.join(workdir, 'gemm3-32out')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '-g', 'perf_model/core/riscv/vector_bytes=512', '-g', 'perf_model/core/riscv/ame_mma_issue_cycles=32',
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None or rc != 0:
        return False, sniper_failed(rc, out)
    rc, out = run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'riscv', 'expected_counts.py'), '--ame', '--ame-n', '32',
                   '--vlen', '4096', '--lmul', '8', '--lam', '4', '--m', '64', '--n', '64', '--k', '64',
                   os.path.join(outdir, 'simulation', 'sim.stats')], 60)
    if rc != 0:
        return False, 'counts off: ' + '; '.join(l for l in out.splitlines() if l.startswith('FAIL'))[:300]
    return True, '64x64x64 on 32x32 tiles (4 tiles, 8 vmmacc.vv) and 32x32 squares (4 squares, 8 mmulbtacc.2d): every count and footprint byte exact'

def case_riscv_llm_size32(p, g, workdir):
    """The agent on 32x32 AME squares (in core and on the tile), 32x32 IME tiles and a 4096-bit vector unit: answers as numpy, counts exact, cycles per token."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    builds = {'ame32': SIZE32_AME_FLAGS, 'ime32': SIZE32_IME_FLAGS}
    guests = {'rvv4k': r['llm']['rvv']}
    for variant, flags in builds.items():
        guest = os.path.join(workdir, 'agent-' + variant)
        rc, out = llm_build(os.environ['SNIPER_RISCV_CC'], guest, flags)
        if rc != 0:
            return False, 'could not build agent.c (%s, exit %s)' % (variant, rc)
        guests[variant] = guest
    runs = [('ame32', 'ica', r['cpu'], 'ame_n=32', ['-g', 'perf_model/core/riscv/ame_mma_issue_cycles=32']),
            ('ame32', 'nca', r['cpu'], 'ame_n=32', ['-g', 'perf_model/ame_tile/coupling=nca']),
            ('ime32', 'ica', SIZE32_CPU, None, ['-g', 'perf_model/core/riscv/vector_bytes=512', '-g', 'perf_model/core/riscv/ime_mma_fp_issue_cycles=32']),
            ('rvv4k', 'ica', SIZE32_CPU, None, ['-g', 'perf_model/core/riscv/vector_bytes=512'])]
    report = []
    traced = {}
    for variant, coupling, cpu, popt, gargs in runs:
        prefix = os.path.join(workdir, 'llm32-' + variant)
        if variant not in traced:
            rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', cpu, '-plugin', '%s,output=%s%s' % (p['frontend'], prefix, (',' + popt) if popt else ''),
                           '--', guests[variant]], 900, cwd=workdir)
            if rc != 100:
                if 'VLEN in the range' in out:
                    return None, 'this qemu-riscv64 caps VLEN below 4096 (rebuild with the current IME patch)'
                return False, '%s: guest exited %s (100 = every line as numpy, every answer right)' % (variant, rc)
            m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', out)
            mb = re.search(r'prefill batches:((?: \d+)+)', out)
            if not m or not mb:
                return False, '%s: no token or batch counts in the guest output' % variant
            traced[variant] = (int(m.group(1)), int(m.group(2)), [int(t) for t in mb.group(1).split()])
        prefilled, generated, batches = traced[variant]
        tokens = prefilled + generated
        outdir = os.path.join(workdir, 'llm32-%s-%s' % (variant, coupling))
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir] + gargs + LLM_DRAM_ARGS
                      + ['--traces=' + prefix + '.app0.th0.sift'], 5400, cwd=SNIPER_ROOT)
        if rc is None or rc != 0:
            return False, '%s/%s: %s' % (variant, coupling, sniper_failed(rc, out))
        stats = {}
        for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
            if ' = ' in line:
                k, v = line.split(' = ', 1)
                stats[k.strip()] = v.strip().split(',')[0]
        cycles = int(float(stats.get('performance_model.cycle_count', 0)))
        insns = int(stats.get('performance_model.instruction_count', 0))
        for k, v in sorted(llm_matrix_counts(variant, generated, batches).items()):
            if int(stats.get(k, -1)) != v:
                return False, '%s/%s: %s = %s, the model shape implies %d' % (variant, coupling, k, stats.get(k), v)
        if coupling != 'ica' and int(stats.get('ame_tile.mma', -1)) != llm_matrix_counts(variant, generated, batches).get('riscv.ame_mma', -2):
            return False, '%s/%s: the tile executed %s squares' % (variant, coupling, stats.get('ame_tile.mma'))
        if coupling != 'ica':
            # a 32x32 fp32 square is 32 rows of 128 bytes and every byte of every load
            # and store reaches the tile; rows whose stride is the row length (the
            # padded token blocks) coalesce into one span, so the tile sees at most that many rows
            rows = (int(stats.get('riscv.ame_load', 0)) + int(stats.get('riscv.ame_store', 0))) * 32
            if int(stats.get('ame_tile.bytes', -1)) != rows * 128 or not 0 < int(stats.get('ame_tile.rows', 0)) <= rows:
                return False, '%s/%s: the tile saw %s rows and %s bytes, the squares hold %d rows (%d bytes)' % (variant, coupling, stats.get('ame_tile.rows'), stats.get('ame_tile.bytes'), rows, rows * 128)
        report.append('%s/%s %.0f cycles/token (%.0f insn/token; %s)' % (variant, coupling, cycles / tokens, insns / tokens, dram_per_token(stats, tokens)))
    return True, '%d tokens at %s GB/s: %s; counts exact' % (tokens, LLM_DRAM_GBS, '; '.join(report))

# The vector unit at its full width (test/riscv-vlen-probe): every VLEN QEMU
# accepts, up to the RVV 1.0 limit of 65,536 bits.  VLEN_KNOWN_GOOD is the
# widest length the probe is required to pass at; QEMU's generic-vector
# expansion is wrong above 2 KB operands (family 5 at 4,096 bits) until fixed.
VLEN_KNOWN_GOOD = 65536

def case_riscv_vlen_probe(p, g, workdir):
    """The vector-width probe at every VLEN QEMU accepts: every instruction family agrees with the scalar reference up to VLEN_KNOWN_GOOD."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'vlen_probe')
    rc, out = run([os.environ['SNIPER_RISCV_CC'], '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O2', '-ffreestanding',
                   '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest,
                   os.path.join(SNIPER_ROOT, 'test', 'riscv-vlen-probe', 'vlen_probe.c')], 300)
    if rc != 0:
        return False, 'could not build the probe (%s)' % exit_text(rc)
    results, cap = {}, None
    vlen = 128
    while vlen <= 65536:
        rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', 'max,vlen=%d' % vlen, guest], 120, cwd=workdir)
        m = re.search(r'VLEN in the range \[\d+, (\d+)\]', out or '')
        if m:
            cap = int(m.group(1))
            break
        results[vlen] = rc
        vlen *= 2
    bad = ['%d: family %d step %d' % (v, rc // 10, rc % 10) for v, rc in sorted(results.items()) if rc != 100]
    required_bad = [v for v, rc in results.items() if rc != 100 and v <= VLEN_KNOWN_GOOD]
    good = [v for v, rc in sorted(results.items()) if rc == 100]
    summary = 'every family agrees at %s bits; QEMU cap %s' % (', '.join(str(v) for v in good), cap if cap else 'none below 65536')
    if bad:
        summary += '; wrong above: ' + ', '.join(bad)
    if required_bad:
        return False, 'the probe fails at a length it must pass (%s): %s' % (', '.join(str(v) for v in required_bad), summary)
    return True, summary

def case_riscv_llm_vlen(p, g, workdir):
    """The agent's vector-length-agnostic kernels (RVV, AME, DMA tasks) reproduce the numpy model at 256, 4,096 and 65,536 bits."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    summary = []
    for vlen in (256, 4096, 65536):
        cpu = r['cpu'].replace('vlen=128', 'vlen=%d' % vlen)
        for variant in ('rvv', 'ame', 'dma'):
            cmd = [r['qemu']] + RISCV_VA_RESERVE + ['-cpu', cpu]
            if variant in ('ame', 'dma'):
                cmd += ['-plugin', '%s,output=%s' % (p['frontend'], os.path.join(workdir, 'llmvlen-%s-%d' % (variant, vlen))), '--']
            rc, out = run(cmd + [r['llm'][variant]], 900, cwd=workdir)
            if rc != 100:
                if 'VLEN in the range' in out:
                    return None, 'this qemu-riscv64 caps VLEN below %d (rebuild with the current IME patch)' % vlen
                return False, '%s at %d bits: guest exited %s (100 = every line as numpy, every answer right)' % (variant, vlen, rc)
        summary.append(str(vlen))
    return True, 'rvv, ame and dma kernels agree with numpy at %s bits' % ', '.join(summary)

def case_riscv_footprint_lines(p, g, workdir):
    """A gather over more lines than the trace record holds (test/riscv-vlen-probe/gather.c) at 4,096 and 65,536 bits: recorded spans plus dropped lines equal the lines touched, and the core times the recorded ones."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gather')
    rc, out = run([os.environ['SNIPER_RISCV_CC'], '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O2', '-ffreestanding',
                   '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-DNGATHER=8', '-o', guest,
                   os.path.join(SNIPER_ROOT, 'test', 'riscv-vlen-probe', 'gather.c')], 300)
    if rc != 0:
        return False, 'could not build the gather guest (%s)' % exit_text(rc)
    report = []
    for vlen in (4096, 65536):
        lanes = vlen * 8 // 32
        prefix = os.path.join(workdir, 'gather-%d' % vlen)
        rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', 'max,vlen=%d' % vlen, '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
        if rc != 100:
            if 'VLEN in the range' in out:
                return None, 'this qemu-riscv64 caps VLEN below %d' % vlen
            return False, 'guest exited %s at %d bits (100 = every gathered element right)' % (rc, vlen)
        outdir = os.path.join(workdir, 'gather-%d-out' % vlen)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '-g', 'perf_model/core/riscv/vector_bytes=%d' % (vlen // 8), '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
        if rc is None or rc != 0:
            return False, '%s at %d bits' % (sniper_failed(rc, out), vlen)
        stats = {}
        for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
            if ' = ' in line:
                k, v = line.split(' = ', 1)
                stats[k.strip()] = v.strip().split(',')[0]
        spans, dropped = int(stats.get('riscv.spans', 0)), int(stats.get('riscv.spans_dropped_lines', 0))
        # per gather: `lanes` lines (one per element) -> 32 recorded + the rest dropped; the index and output
        # unit-stride accesses are one contiguous span each (lanes*4 bytes), recorded whole
        want_gather_lines = 8 * lanes
        want_dropped = 8 * (lanes - 32)
        if dropped != want_dropped:
            return False, '%d bits: %d dropped lines, the gathers imply %d' % (vlen, dropped, want_dropped)
        if spans < 8 * 32 + 16:
            return False, '%d bits: only %d spans recorded' % (vlen, spans)
        report.append('%d bits: %d lines per gather, 32 recorded and %d counted beyond each' % (vlen, lanes, lanes - 32))
    return True, '; '.join(report)

def case_riscv_vector_datapath(p, g, workdir):
    """The vector agent at 4,096 bits with the full-width datapath and with a 16-byte one: the narrow unit is slower, the instruction counts identical."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    cpu = r['cpu'].replace('vlen=128', 'vlen=4096')
    prefix = os.path.join(workdir, 'vdp')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', cpu, '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', r['llm']['rvv']], 900, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    cycles, counts = {}, {}
    for width in (0, 16):
        outdir = os.path.join(workdir, 'vdp-%d' % width)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '-g', 'perf_model/core/riscv/vector_bytes=512', '-g', 'perf_model/core/vector/datapath_bytes=%d' % width,
                       '--traces=' + prefix + '.app0.th0.sift'], 3600, cwd=SNIPER_ROOT)
        if rc is None or rc != 0:
            return False, '%s (datapath %d)' % (sniper_failed(rc, out), width)
        stats = {}
        for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
            if ' = ' in line:
                k, v = line.split(' = ', 1)
                stats[k.strip()] = v.strip().split(',')[0]
        cycles[width] = int(float(stats['performance_model.cycle_count']))
        counts[width] = tuple(stats.get(k) for k in ('riscv.v_load', 'riscv.v_fp_muladd', 'performance_model.instruction_count'))
    if counts[0] != counts[16]:
        return False, 'the counts differ between datapath widths: %s vs %s' % (counts[0], counts[16])
    if cycles[16] <= cycles[0]:
        return False, 'a 16-byte datapath (%d cycles) is not slower than the full width (%d)' % (cycles[16], cycles[0])
    return True, 'full width %d cycles, 16-byte datapath %d cycles (%.2fx), counts identical' % (cycles[0], cycles[16], cycles[16] / cycles[0])

def case_riscv_llm_wide(p, g, workdir):
    """The 100 MiB agent (test/llm-agent-wide, generated on the host) on the two fast configurations of tools/llm/wide-sweep.sh: fp32 on 32x32 AME squares and int4 on a 4,096-bit vector unit, under LLM_DRAM_GBS."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    wide = os.path.join(SNIPER_ROOT, 'test', 'llm-agent-wide')
    if not os.path.isfile(os.path.join(wide, 'model_weights.bin')) or not os.path.isfile(os.path.join(wide, 'model_w4.bin')):
        return None, 'no wide model (see test/llm-agent-wide/README.md)'
    env = dict(os.environ, SNIPER_QEMU_RISCV=r['qemu'])
    try:
        # two recordings of ~150 M instructions and two simulations: about two hours on an idle machine
        proc = subprocess.run(['bash', os.path.join(SNIPER_ROOT, 'tools', 'llm', 'wide-sweep.sh'), workdir, str(LLM_DRAM_GBS), 'fp32-ame32', 'w4-rvv4k'],
                              cwd=SNIPER_ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=14400)
    except subprocess.TimeoutExpired:
        return False, 'the sweep did not finish within four hours'
    lines = [l for l in proc.stdout.splitlines() if 'cycles/token' in l]
    if len(lines) != 2:
        return False, 'sweep output: ' + proc.stdout.strip()[-300:]
    for l in lines:
        m = re.search(r'dram/token (\d+) reads \+ (\d+) writes', l)
        if not m:
            return False, 'no DRAM figures in: ' + l
        reads, writes = int(m.group(1)), int(m.group(2))
        # the weights stream in once per token and are never written: writes are
        # the activations' evictions, a small fraction (clean lines must not be written back)
        if writes * 8 > reads:
            return False, 'DRAM writes (%d/token) are not small against reads (%d/token): clean lines written back?' % (writes, reads)
    return True, '; '.join(' '.join(l.split()) for l in lines)

def gdb_read_feature(port, feature, names, timeout=30):
    """Attach to a gdbstub, fetch the target description, and read the named
    registers of one feature the way gdb would: (features, regs, values)."""
    deadline = time.time() + timeout
    s = None
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=timeout)
            break
        except OSError:
            time.sleep(0.2)
    if s is None:
        raise RuntimeError('no gdbstub on port %d' % port)
    def recv():
        data = b''
        while b'#' not in data or len(data) < data.index(b'#') + 3:
            data += s.recv(4096)
        s.sendall(b'+')
        return data[data.index(b'$') + 1:data.index(b'#')].decode('latin1')
    def pkt(p):
        s.sendall(('$%s#%02x' % (p, sum(p.encode()) % 256)).encode())
        return recv()
    def xfer(name):
        out, off = '', 0
        while True:
            r = pkt('qXfer:features:read:%s:%x,fff' % (name, off))
            out += r[1:]
            off += len(r) - 1
            if r[:1] == 'l':
                return out
    try:
        s.sendall(b'+')
        pkt('qSupported:xmlRegisters=riscv;qXfer:features:read+')
        feats = re.findall(r'href="([^"]+)"', xfer('target.xml'))
        xml = xfer(feature + '.xml') if feature + '.xml' in feats else ''
        regs = re.findall(r'<reg name="([^"]+)" bitsize="(\d+)" regnum="(\d+)"', xml)
        values = {}
        for name, bits, num in regs:
            if name in names:
                values[name] = pkt('p%x' % int(num))
        s.sendall(b'$k#6b')
    finally:
        s.close()
    return feats, regs, values

def case_riscv_plugin_api(p, g, workdir):
    """The plugin-API additions of the QEMU patch: instructions claimed at translation, exceptions the plugin's model raises, the AME CSRs it backs, and the unit's registers in the vcpu's gdb list."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    cc = os.environ['SNIPER_RISCV_CC']
    def build(src, out, extra):
        return run([cc, '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O2', '-ffreestanding',
                    '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-I', os.path.join(SNIPER_ROOT, 'include')]
                   + extra + ['-o', out, os.path.join(PROBE_DIR, src)], 120)
    guests = {}
    for mode in (0, 1, 2):
        guests[mode] = os.path.join(workdir, 'rv-ame-api%d' % mode)
        rc, out = build('rv-ame-api.c', guests[mode], ['-DMODE=%d' % mode])
        if rc != 0:
            return False, 'could not build rv-ame-api.c mode %d (%s)' % (mode, exit_text(rc))
    loop = os.path.join(workdir, 'rv-ame-loop')
    rc, out = build('rv-ame-loop.c', loop, [])
    if rc != 0:
        return False, 'could not build rv-ame-loop.c (%s)' % exit_text(rc)
    def plug(opts):
        return [r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s%s' % (p['frontend'], os.path.join(workdir, 'api'), opts), '--']
    def emulated(out):
        m = re.search(r'plugin-emulated instructions: (\d+) claimed at translation, (\d+) through the exception path', out)
        return (int(m.group(1)), int(m.group(2))) if m else None
    # the probe through claimed instructions: every check passes, the CSRs answer, nothing trapped
    rc, out = run(plug('') + [guests[0]], 120, cwd=workdir)
    if rc != 100:
        return False, 'the API probe exited %s with the plugin (100 = every check passed): %s' % (rc, out.strip()[-200:])
    if emulated(out) != (11, 0):
        return False, 'claimed run: expected 11 instructions claimed and 0 trapped, got %s' % (emulated(out),)
    m = re.search(r'AME registers: (\d+) in the gdbstub list under org.riscv.ame, amenlen reads (\d+)', out)
    if not m or int(m.group(1)) != 103 or int(m.group(2)) != 4:
        return False, 'the unit registers are not in the vcpu register list: %s' % (m.group(0) if m else 'no report')
    # the same through the exception path (claim=0): every instruction trapped and re-entered
    rc, out = run(plug(',claim=0') + [guests[0]], 120, cwd=workdir)
    if rc != 100 or emulated(out) != (0, 11):
        return False, 'exception path: exit %s, %s' % (rc, emulated(out))
    # without the plugin QEMU does not know the CSRs
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], guests[0]], 60, cwd=workdir)
    if rc != -signal.SIGILL:
        return False, 'without the plugin the CSR read should be an illegal instruction (%s)' % exit_text(rc)
    # the model's exceptions reach the guest as the architecture's signals
    rc, out = run(plug('') + [guests[1]], 60, cwd=workdir)
    if rc != -signal.SIGILL or 'without owning' not in out:
        return False, 'use without ownership: exit %s, expected SIGILL with the model\'s report' % rc
    rc, out = run(plug('') + [guests[2]], 60, cwd=workdir)
    if rc != -signal.SIGSEGV or 'failed at 0x10' not in out:
        return False, 'a square load from an unmapped address: exit %s, expected SIGSEGV with the model\'s report' % rc
    # the registers over the gdb protocol
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
    sock.close()
    proc = subprocess.Popen([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-g', str(port), '-plugin', '%s,output=%s' % (p['frontend'], os.path.join(workdir, 'apigdb')), '--', guests[0]],
                            cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
    try:
        feats, regs, values = gdb_read_feature(port, 'org.riscv.ame', ('amenlen', 'ameudsz', 'm0'))
    except Exception as e:
        return False, 'gdb protocol: %s' % e
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        proc.communicate()
    if 'org.riscv.ame.xml' not in feats:
        return False, 'gdb target description lacks the plugin feature: %s' % feats
    if len(regs) != 103 or values.get('amenlen') != '0400000000000000' or values.get('ameudsz') != '2000000000000000' or len(values.get('m0', '')) != 128:
        return False, 'gdb: %d registers, amenlen %s, ameudsz %s, m0 %d hex digits' % (len(regs), values.get('amenlen'), values.get('ameudsz'), len(values.get('m0', ''))) + versions_note(r['qemu'])
    # the mechanism's cost: 4 M emulated instructions, no tracing, claimed and trapped
    times = {}
    for opt, want in ((',trace=0,claim=1', (4000000, 0)), (',trace=0,claim=0', (0, 4000000))):
        started = time.time()
        rc, out = run(plug(opt) + [loop], 300, cwd=workdir)
        times[opt] = time.time() - started
        if rc != 100 or emulated(out) != want:
            return False, 'loop guest%s: exit %s, %s' % (opt, rc, emulated(out))
    return True, ('11 claimed / 0 trapped and 0 / 11; SIGILL and SIGSEGV delivered with the model\'s report; 103 registers under org.riscv.ame over the gdb protocol (amenlen 4); '
                  '4 M emulated instructions: %.2f s claimed, %.2f s through the exception path' % (times[',trace=0,claim=1'], times[',trace=0,claim=0']))

def case_riscv_ame_couplings(p, g, workdir):
    """The attached tile (AmeTile) runs a 32x32x32 AME product under every coupling: tile counts exact, timing responds."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gemm3-ame32')
    REPEAT = 16
    rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest, ['-Wl,-e,_start', '-DUSE_AME', '-DM=32', '-DN=32', '-DK=32', '-DAME_REPEAT=%d' % REPEAT])
    if rc != 0:
        return False, 'could not build the 32-cubed AME variant (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'gemm3ame32')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    if rc != 100:
        return False, 'guest exited %s (100 = scalar, RVV, IME and AME agree)' % rc
    # 4x4 Int32 squares over 32x32x32: 64 output squares, 8 K slices each; a
    # square row is 16 bytes at a 128-byte stride, so every row is its own
    # cache line and reaches the tile as one access.
    squares, ks = 64, 8
    loads, stores, mma, moves, cfg, own = 2 * squares * ks, squares, squares * ks, squares, 3 + squares, 2
    loads, stores, mma, moves, cfg, own = (x * REPEAT for x in (loads, stores, mma, moves, cfg, own))
    rows = (loads + stores) * 4
    ops = rows + (loads + stores) + mma + moves + cfg + own     # a memory instruction also has one execute micro-op
    cycles = {}
    l1 = {}
    for coupling in ('ica', 'oca_l1', 'oca_l2', 'oca_llc', 'nca'):
        outdir = os.path.join(workdir, 'tile-' + coupling)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '-g', 'perf_model/ame_tile/coupling=' + coupling, '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
        if rc is None or rc != 0:
            return False, '%s: %s' % (coupling, sniper_failed(rc, out))
        stats = {}
        for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
            if ' = ' in line:
                k, v = line.split(' = ', 1)
                stats[k.strip()] = v.strip().split(',')[0]
        cycles[coupling] = int(float(stats['performance_model.cycle_count']))
        l1[coupling] = (int(stats.get('L1-D.loads', 0)), int(stats.get('L1-D.stores', 0)))
        if int(stats.get('riscv.ame_mma', 0)) != mma or int(stats.get('riscv.ame_load', 0)) != loads:
            return False, '%s: AME class counts off (%s mma, %s loads)' % (coupling, stats.get('riscv.ame_mma'), stats.get('riscv.ame_load'))
        if coupling == 'ica':
            if 'ame_tile.ops' in stats:
                return False, 'ica must not attach a tile'
            continue
        want = {'ame_tile.ops': ops, 'ame_tile.rows': rows, 'ame_tile.mma': mma, 'ame_tile.bytes': rows * 16,
                'ame_tile.prefetch_hits': loads * 4 if coupling == 'nca' else 0}
        for k, v in want.items():
            if int(stats.get(k, -1)) != v:
                return False, '%s: %s = %s, the square geometry implies %d' % (coupling, k, stats.get(k), v)
        # Where the tile's port enters: at L1 its rows are L1 accesses, as in
        # the core; at L2, the LLC or near-core they bypass the L1 data cache,
        # every load row and every store row.  (This was an if-statement with
        # an empty body.)
        bypass = (0, 0) if coupling == 'oca_l1' else (loads * 4, stores * 4)
        seen = (l1['ica'][0] - l1[coupling][0], l1['ica'][1] - l1[coupling][1])
        if seen != bypass:
            return False, ('%s: the L1 data cache saw %d load and %d store rows fewer than in the core, '
                           'expected %d and %d' % ((coupling,) + seen + bypass))
    if len(set(cycles.values())) < 2:
        return False, 'the couplings all take %d cycles: the tile timing does not reach the core' % cycles['ica']
    return True, 'tile counts exact under every coupling (%d ops, %d rows, %d squares); cycles %s' % (
        ops, rows, mma, ', '.join('%s %d' % (c, n) for c, n in cycles.items()))


# --- code rewritten in place, and run-sniper driving QEMU ----------------------

def sim_instructions(outdir):
    for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
        if line.split(' = ')[0].strip() == 'core.instructions':
            return int(line.split(' = ', 1)[1].split(',')[0])
    return 0


def simulate_trace(trace, cfg, outdir):
    """run-sniper on one stored trace: (instructions, None) or (None, why)."""
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', cfg, '-d', outdir,
                   '--traces=' + trace], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return None, 'simulating %s on %s failed (exit %s)' % (os.path.basename(trace), cfg, rc)
    return sim_instructions(outdir), None


def first_bytes_at(p, trace, pc, n=2):
    """The first n bytes of every instruction the trace has at pc, in order."""
    rc, dump = run([p['siftdump'], trace], 300)
    if rc != 0:
        return None
    pc = pc.lstrip('0')
    return [''.join(l.split()[1:1 + n]) for l in dump.splitlines()
            if l.split() and l.split()[0].lstrip('0') == pc and not l.startswith('magic')]


def smc_verdict(seen, want, what):
    """smc=1 must trace each run's own bytes; smc=0, the stale ones."""
    if seen['1'] != want:
        return False, '%s: traced %s at the rewritten address, expected %s' % (what, seen['1'], want)
    if seen['0'] == want:
        return False, '%s: smc=0 traced the new bytes too: the option has no effect' % what
    return True, None


def case_system_smc_rewrite(p, g, workdir):
    """Code rewritten in place in a whole x86 machine.

    The boot sector (bootsmc.S) copies code A to 0x8000 and calls it, copies
    code B over it -- a different first instruction, of a different length --
    and calls it, then A again, in real mode.  With smc=1 the trace carries A,
    B, A at 0x8000 and simulates; smc=0 must give the stale A, A, A."""
    seen = {}
    for smc in ('1', '0'):
        prefix = os.path.join(workdir, 'xsmc%s' % smc)
        rc, out = run([p['system'], '-nographic', '-display', 'none', '-accel', 'tcg', '-m', '128',
                       '-drive', 'file=%s,format=raw,if=floppy' % g['bootsmc.img'],
                       '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-no-reboot',
                       '-plugin', '%s,output=%s,smc=%s' % (p['frontend'], prefix, smc)], 300, cwd=workdir)
        if rc != 33:
            return False, 'the guest failed its own checks (exit %s, smc=%s)' % (rc, smc)
        seen[smc] = first_bytes_at(p, prefix + '.app0.th0.sift', '8000')
    ok, why = smc_verdict(seen, ['b801', '31c0', 'b801'], 'x86 full system')
    if not ok:
        return False, why
    n, why = simulate_trace(os.path.join(workdir, 'xsmc1.app0.th0.sift'), 'gainestown', os.path.join(workdir, 'xsmc-sim'))
    if n is None:
        return False, why
    return True, 'traced A, B, A at 0x8000 (smc=0: %s); %d instructions simulated' % (' '.join(seen['0']), n)


def case_riscv_system_smc_rewrite(p, g, workdir):
    """Code rewritten in place on a bare-metal RISC-V machine (rv-bare-smc.c):
    A, then compressed B at the same address, then A, with fence.i between.
    smc=1 traces each run's own bytes and simulates; smc=0 the stale ones."""
    r, why = riscv_system_guest(workdir, 'rv-bare-smc', ['rv-bare-smc.c'])
    if r is None:
        return None, why
    seen = {}
    for smc in ('1', '0'):
        prefix = os.path.join(workdir, 'rsmc%s' % smc)
        rc, out = run([r['qemu'], '-M', 'virt', '-bios', 'none', '-nographic', '-kernel', r['guest'],
                       '-plugin', '%s,output=%s,smc=%s' % (p['frontend'], prefix, smc)], 300, cwd=workdir)
        if rc != 0:
            return False, 'the guest failed its own checks (exit %s, smc=%s)' % (rc, smc)
        m = re.search(r'B rewritten pc=0x([0-9a-f]+) returned=2', out)
        if not m:
            return False, 'the guest did not run code B correctly (smc=%s)' % smc
        seen[smc] = first_bytes_at(p, prefix + '.app0.th0.sift', m.group(1))
    ok, why = smc_verdict(seen, ['1305', '0945', '1305'], 'RISC-V full system')
    if not ok:
        return False, why
    n, why = simulate_trace(os.path.join(workdir, 'rsmc1.app0.th0.sift'), 'riscv', os.path.join(workdir, 'rsmc-sim'))
    if n is None:
        return False, why
    return True, 'traced A, B, A (smc=0: %s); %d instructions simulated' % (' '.join(seen['0']), n)


def aarch64_tools():
    qemu = os.environ.get('SNIPER_QEMU_AARCH64') or os.path.join(SNIPER_ROOT, 'qemu-aarch64', 'build', 'qemu-aarch64')
    cc = os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')
    if not os.path.isfile(qemu):
        return None, None, 'no qemu-aarch64 (make ARM=ON QEMU=ON, or set SNIPER_QEMU_AARCH64)'
    if not cc or not os.path.isfile(cc):
        return None, None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    return qemu, cc, None


def case_aarch64_core_configs(p, g, workdir):
    """The ARM cores of the core-aggressiveness sweep (config/core_configs/
    cortex_a53, cortex_a55, cortex_a720, cortex_x4, oryon_xelite) on an
    AArch64 trace.  They used the Nehalem core model and the Pentium M
    predictor -- x86 cores with ARM sizes, which could not run an ARM trace.
    Built on the cortex-a53 and cortex-a72 models, each simulates a64-loop
    (to 0.1%, as the other AArch64 cases) with its L1-D prefetcher at work, and the
    wide cores' issue pipelines (perf_model/core/cortex_a72_ports) show: the
    X4 is faster than the A720, which is faster than the A72, whose result
    the configurable ports leave as it was; the in-order cores are slower
    than all of them."""
    qemu, cc, why = aarch64_tools()
    if why:
        return None, why
    guest = os.path.join(workdir, 'a64-loop')
    rc, out = run([cc, '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-nostdlib', '-static',
                   '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, os.path.join(PROBE_DIR, 'a64-loop.c')], 120)
    if rc != 0:
        return None, 'could not build a64-loop.c (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'a64-loop')
    rc, out = run([qemu, '-cpu', A64_CPU, '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest],
                  300, cwd=workdir)
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    if rc != 0 or not m:
        return False, 'recording a64-loop failed (%s)' % exit_text(rc)
    written = int(m.group(1))
    ipc = {}
    for cfg in ('cortex-a72', 'core_configs/cortex_a53', 'core_configs/cortex_a55', 'core_configs/cortex_a720',
                'core_configs/cortex_x4', 'core_configs/oryon_xelite'):
        name = cfg.split('/')[-1]
        outdir = os.path.join(workdir, 'cc-' + name)
        n, why = simulate_trace(prefix + '.app0.th0.sift', cfg, outdir)
        if n is None:
            return False, why
        if abs(n - written) > written // 1000:
            return False, '%s simulated %d instructions of the %d traced' % (name, n, written)
        st = stats_of(outdir)
        if cfg != 'cortex-a72' and not int(float(st.get('ip_stride.gen_prefetch', 0))):
            return False, '%s: the L1-D ip_stride prefetcher generated nothing' % name
        ipc[name] = n / float(st['performance_model.cycle_count'])
    order = ['cortex_x4', 'cortex_a720', 'cortex-a72', 'cortex_a55']
    if not all(ipc[a] > ipc[b] for a, b in zip(order, order[1:])) or not ipc['cortex-a72'] > ipc['cortex_a53']:
        return False, 'IPCs out of order: %s' % ', '.join('%s %.2f' % kv for kv in sorted(ipc.items()))
    return True, '%d instructions traced; IPC %s' % (written, ', '.join('%s %.2f' % (k, ipc[k]) for k in
                  ('cortex_x4', 'oryon_xelite', 'cortex_a720', 'cortex-a72', 'cortex_a55', 'cortex_a53')))


def case_aarch64_smc_rewrite(p, g, workdir):
    """Code rewritten in place on AArch64 (a64-smc-rewrite.c): A, then B at
    the same address, then A, with the architecture's cache maintenance
    between.  smc=1 traces each run's own bytes and simulates on cortex-a72;
    smc=0 the stale ones."""
    qemu, cc, why = aarch64_tools()
    if why:
        return None, why
    guest = os.path.join(workdir, 'a64-smc-rewrite')
    rc, out = run([cc, '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-nostdlib', '-static',
                   '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, os.path.join(PROBE_DIR, 'a64-smc-rewrite.c')], 120)
    if rc != 0:
        return None, 'could not build a64-smc-rewrite.c (%s)' % exit_text(rc)
    seen = {}
    for smc in ('1', '0'):
        prefix = os.path.join(workdir, 'asmc%s' % smc)
        rc, out = run([qemu, '-cpu', A64_CPU, '-plugin', '%s,output=%s,smc=%s' % (p['frontend'], prefix, smc), '--', guest],
                      300, cwd=workdir)
        if rc != 0:
            return False, 'qemu-aarch64 exited %s (smc=%s)' % (rc, smc)
        if re.findall(r'returned=(\d)', out) != ['1', '2', '1']:
            return False, 'the guest did not run A, B, A correctly (smc=%s)' % smc
        pc = re.search(r'B rewritten pc=0x([0-9a-f]+)', out).group(1)
        seen[smc] = first_bytes_at(p, prefix + '.app0.th0.sift', pc)
    ok, why = smc_verdict(seen, ['2000', '4000', '2000'], 'AArch64')
    if not ok:
        return False, why
    n, why = simulate_trace(os.path.join(workdir, 'asmc1.app0.th0.sift'), 'cortex-a72', os.path.join(workdir, 'asmc-sim'))
    if n is None:
        if 'built without the armv8 decoder' in (why or ''):
            return None, 'Sniper built without the ARM decoder (make ARM=ON)'
        return False, why
    return True, 'traced A, B, A (smc=0: %s); %d instructions simulated' % (' '.join(seen['0']), n)


def case_aarch64_sve_simulate(p, g, workdir):
    """SVE: a guest built with +sve (a64-sve.c -- predicated loads and stores,
    multiply-adds, a reduction, a gather) runs on QEMU's "max" CPU, which has
    SVE, and gives the same answer as its build without SVE; its trace
    simulates on cortex-a72 and cortex-a53 instruction for instruction.
    Capstone 5 decodes SVE; the decoder gives its Z and P operands their read
    and write roles (Capstone leaves them unset), so dependencies run through
    them."""
    qemu, cc, why = aarch64_tools()
    if why:
        return None, why
    src = os.path.join(PROBE_DIR, 'a64-sve.c')
    builds = {}
    for name, march in (('a64-sve', 'armv8.2-a+sve'), ('a64-nosve', 'armv8.2-a')):
        out = os.path.join(workdir, name)
        rc, o = run([cc, '--target=aarch64-linux-gnu', '-O3', '-march=' + march, '-ffreestanding', '-nostdlib',
                     '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', out, src], 120)
        if rc != 0:
            return None, 'could not build %s (%s)' % (name, exit_text(rc))
        builds[name] = out
    rc, plain = run([qemu, '-cpu', A64_CPU, builds['a64-nosve']], 120)
    answer = re.search(r'sve sum=\d+ f=\d+', plain)
    if rc != 0 or not answer:
        return False, 'the build without SVE failed (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'sve')
    rc, out = run([qemu, '-cpu', A64_CPU, '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', builds['a64-sve']],
                  300, cwd=workdir)
    if rc != 0 or answer.group(0) not in out:
        return False, 'the SVE build gave a different answer, or failed (%s)' % exit_text(rc)
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    written = int(m.group(1)) if m else 0
    summary = []
    for cfg in ('cortex-a72', 'cortex-a53'):
        n, why = simulate_trace(prefix + '.app0.th0.sift', cfg, os.path.join(workdir, 'sve-' + cfg))
        if n is None:
            if 'built without the armv8 decoder' in (why or ''):
                return None, 'Sniper built without the ARM decoder (make ARM=ON)'
            return False, why
        if not written or abs(n - written) > written // 1000:
            return False, '%s simulated %d instructions of the %d traced' % (cfg, n, written)
        summary.append(cfg)
    # Recorded at QEMU's default SVE width, 64 bytes, the trace is refused by
    # a configuration that models 16, with the reason; it used to be timed at
    # the wrong width without a word
    prefix64 = os.path.join(workdir, 'sve64')
    rc, out = run([qemu, '-cpu', 'max', '-plugin', '%s,output=%s' % (p['frontend'], prefix64), '--', builds['a64-sve']],
                  300, cwd=workdir)
    if rc != 0:
        return False, 'the SVE build failed at QEMU\'s default vector length (%s)' % exit_text(rc)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'cortex-a72', '-d', os.path.join(workdir, 'sve64-a72'),
                   '--traces=' + prefix64 + '.app0.th0.sift'], 600, cwd=SNIPER_ROOT)
    if rc == 0 or "The guest's vector registers are 64 bytes" not in out:
        return False, 'a 64-byte SVE recording on a 16-byte configuration was not refused (%s)' % exit_text(rc)
    return True, '%s; %d instructions simulated on %s; a 64-byte recording refused' % (answer.group(0), written, ' and '.join(summary))


def pacsim_case(arch, workdir, mode='single'):
    """testing/test_pacsim.py --frontend qemu: the sampler in the plugin, on a
    live channel, against the full run (accuracy, detail share, checksum)."""
    cmd = [sys.executable, os.path.join(SNIPER_ROOT, 'testing', 'test_pacsim.py'), '--frontend', 'qemu',
           '--arch', arch, '--profile', 'quick', '--mode', mode]
    rc, out = run(cmd, 3600, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'timed out'
    if out.startswith('SKIP:') or '\nSKIP:' in out:
        return None, re.search(r'SKIP: (.*)', out).group(1)
    verdicts = re.findall(r'^\s+(\S+)\s+(PASS|FAIL|XFAIL|XPASS|SKIP)\b(.*)$', out, re.M)
    if rc != 0 or not verdicts:
        return False, 'test_pacsim.py exited %s: %s' % (rc, out.strip().splitlines()[-1][:160] if out.strip() else '')
    acc = [d for n, v, d in verdicts if n == 'accurate']
    return True, ', '.join('%s %s' % (n, v) for n, v, d in verdicts) + (' (' + acc[0].strip()[:90] + ')' if acc else '')


def case_user_pacsim(p, g, workdir):
    """Pac-Sim through the plugin, x86-64 user mode, single-threaded."""
    return pacsim_case('x86_64', workdir)


def case_riscv_user_pacsim(p, g, workdir):
    """Pac-Sim through the plugin, RISC-V user mode, single-threaded."""
    if not os.environ.get('SNIPER_RISCV_CC'):
        return None, 'SNIPER_RISCV_CC not set to a RISC-V compiler'
    return pacsim_case('riscv64', workdir)


def case_aarch64_user_pacsim(p, g, workdir):
    """Pac-Sim through the plugin, AArch64 user mode, single-threaded, on cortex-a72."""
    if not (os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')):
        return None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    return pacsim_case('aarch64', workdir)


# Threads: the plugin waits for Sniper outside QEMU's run loop, which needs
# the wait call in the QEMU (tools/qemu-11.1.1-plugin-wait.patch); a QEMU
# without it refuses the second thread (user-mt-live and the *-user-mt cases
# check that), and the sampling cases are skipped, since it is the QEMU that
# is missing something.
def case_user_pacsim_mt(p, g, workdir):
    """Pac-Sim on qemu-x86_64 with threads: the barrier shape, four workers on their own cores."""
    if not qemu_can_wait(user_qemu('x86_64')):
        return None, '%s lacks qemu_plugin_vcpu_wait_at (tools/qemu-11.1.1-plugin-wait.patch)' % user_qemu('x86_64')
    return pacsim_case('x86_64', workdir, mode='barrier')


def case_riscv_user_pacsim_mt(p, g, workdir):
    """Pac-Sim on qemu-riscv64 with threads: the barrier shape, freestanding, its own futex barrier."""
    if not os.environ.get('SNIPER_RISCV_CC'):
        return None, 'SNIPER_RISCV_CC not set to a RISC-V compiler'
    if not qemu_can_wait(user_qemu('riscv64')):
        return None, '%s lacks qemu_plugin_vcpu_wait_at (tools/qemu-11.1.1-plugin-wait.patch)' % user_qemu('riscv64')
    return pacsim_case('riscv64', workdir, mode='barrier')


def case_aarch64_user_pacsim_mt(p, g, workdir):
    """Pac-Sim on qemu-aarch64 with threads: the same freestanding barrier shape, on cortex-a72."""
    if not (os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')):
        return None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    if not qemu_can_wait(user_qemu('aarch64')):
        return None, '%s lacks qemu_plugin_vcpu_wait_at (tools/qemu-11.1.1-plugin-wait.patch)' % user_qemu('aarch64')
    return pacsim_case('aarch64', workdir, mode='barrier')


ROI_SUM = 'sum=2152578694144'    # roi.c and rv-bare-roi.c: 5 + 10 + 5 rounds of work


def roi_guests(workdir):
    """roi.c for each architecture there is a QEMU and a compiler for:
    [(arch, qemu, guest, config)], and the reasons for any left out."""
    out, missing = [], []
    clang = os.environ.get('SNIPER_RISCV_CC')
    qx = os.environ.get('SNIPER_QEMU_X86_64') or os.path.join(SNIPER_ROOT, 'qemu', 'build', 'qemu-x86_64')
    builds = [('x86_64', qx, 'gainestown', [])] if os.path.isfile(qx) else []
    if not builds:
        missing.append('x86_64: no qemu-x86_64')
    qa, ca, why = aarch64_tools()
    if why:
        missing.append('aarch64: ' + why)
    else:
        builds.append(('aarch64', qa, 'cortex-a72', []))
    qr = os.environ.get('SNIPER_QEMU_RISCV') or os.path.join(SNIPER_ROOT, 'qemu-riscv', 'build', 'qemu-riscv64')
    if not os.path.isfile(qr) or not clang:
        missing.append('riscv64: no qemu-riscv64 or SNIPER_RISCV_CC')
    else:
        builds.append(('riscv64', qr, 'riscv', ['-march=rv64gc', '-mabi=lp64d']))
    for arch, qemu, cfg, flags in builds:
        cc = clang if clang and os.path.isfile(clang) else 'cc'
        guest = os.path.join(workdir, 'roi-' + arch)
        cmd = [cc] + (['--target=%s-linux-gnu' % arch, '-fuse-ld=lld'] if cc != 'cc' else []) + flags + \
              ['-O1', '-ffreestanding', '-nostdlib', '-static', '-Wl,-e,_start',
               '-I', os.path.join(SNIPER_ROOT, 'include'), '-o', guest, os.path.join(PROBE_DIR, 'roi.c')]
        rc, o = run(cmd, 120)
        if rc != 0:
            missing.append('%s: could not build roi.c' % arch)
            continue
        out.append((arch, qemu, guest, cfg))
    return out, missing


def case_run_sniper_qemu_roi(p, g, workdir):
    """run-sniper drives QEMU user mode, live, with a region of interest.

    roi.c brackets ten of twenty rounds of work with SimRoiStart() and
    SimRoiEnd().  For each architecture: the front end's own trace with roi=1
    holds about half the instructions of one without, with the markers (1, 2)
    and the SimInSimulator() query (8) in order; then run-sniper
    --frontend=qemu --roi runs the guest live, the guest learns it is
    simulated (the answer reached its register), and the simulator counts the
    ROI's instructions, to 0.1%."""
    guests, missing = roi_guests(workdir)
    summary = []
    for arch, qemu, guest, cfg in guests:
        counts = {}
        for roi in ('0', '1'):
            prefix = os.path.join(workdir, 'roi-%s-%s' % (arch, roi))
            rc, out = run([qemu, '-plugin', '%s,output=%s,roi=%s' % (p['frontend'], prefix, roi), '--', guest],
                          300, cwd=workdir)
            if rc != 0 or ROI_SUM not in out:
                return False, '%s: the guest failed under QEMU (exit %s, roi=%s)' % (arch, rc, roi)
            m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
            counts[roi] = int(m.group(1)) if m else 0
            rc, dump = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
            magic = [l.split()[1] for l in dump.splitlines() if l.startswith('magic ')]
            # A guest with vector registers opens its trace with the plugin's
            # report of their width (29, SIM_CMD_VECTOR_BYTES); within a region
            # of interest it may come before the region, outside the trace
            if arch != 'x86_64' and magic[:1] == ['29']:
                magic = magic[1:]
            elif arch != 'x86_64' and roi == '0':
                return False, '%s: no vector width record (29) opens the trace: %s' % (arch, magic)
            if magic != ['1', '2', '8']:
                return False, '%s: magic records %s in the trace, expected 1 2 8 (roi=%s)' % (arch, magic, roi)
        share = counts['1'] / float(counts['0'] or 1)
        if not 0.45 <= share <= 0.55:
            return False, '%s: the ROI trace holds %.0f%% of the instructions, expected about half' % (arch, 100 * share)
        outdir = os.path.join(workdir, 'roi-%s-live' % arch)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', cfg, '-d', outdir,
                       '--frontend=qemu', '--roi', '--', guest], 900, cwd=SNIPER_ROOT)
        if rc != 0:
            return False, '%s: run-sniper --frontend=qemu exited %s' % (arch, rc)
        if 'roi in_sim=1 ' + ROI_SUM not in out:
            return False, '%s: the guest did not see the simulator answer (or computed the wrong sum)' % arch
        n = sim_instructions(outdir)
        if abs(n - counts['1']) > counts['1'] // 1000:
            return False, '%s: simulated %d instructions, the ROI holds %d' % (arch, n, counts['1'])
        summary.append('%s %d of %d' % (arch, n, counts['0']))
    if not summary:
        return None, '; '.join(missing)
    return True, 'ROI simulated live: ' + ', '.join(summary) + ('' if not missing else ' (not run: %s)' % '; '.join(missing))


def case_run_sniper_qemu_riscv_units(p, g, workdir):
    """run-sniper --frontend=qemu on the RVV/IME and the AME GEMM guests, live,
    with no CPU option: record-trace's RISC-V default enables the vector and
    matrix units, and every class count and footprint byte matches the
    specification exactly as it does from a trace (riscv-gemm-simulate,
    riscv-ame-simulate)."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    ame = os.path.join(workdir, 'gemm3-ame')
    if not os.path.isfile(ame):
        rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], ame, ['-Wl,-e,_start', '-DUSE_AME'])
        if rc != 0:
            return None, 'could not build gemm3.c with AME (%s)' % exit_text(rc)
    summary = []
    for name, guest, flags in (('RVV/IME', r['gemm'], []), ('AME', ame, ['--ame'])):
        outdir = os.path.join(workdir, 'live-' + name.replace('/', '-'))
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '--frontend=qemu', '--', guest], 900, cwd=SNIPER_ROOT,
                      env=dict(os.environ, SNIPER_QEMU_RISCV=r['qemu']))
        if rc != 0:
            return False, '%s: run-sniper --frontend=qemu exited %s' % (name, rc)
        if 'undecoded instruction' in out:
            return False, '%s: %s' % (name, re.search(r'undecoded instruction[^\n]*', out).group(0))
        rc, out = run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'riscv', 'expected_counts.py')] + flags
                      + [os.path.join(outdir, 'simulation', 'sim.stats')], 60)
        if rc != 0:
            return False, '%s: counts off: %s' % (name, '; '.join(l for l in out.splitlines() if l.startswith('FAIL'))[:200])
        summary.append('%s %d counts exact' % (name, sum(1 for l in out.splitlines() if l.startswith('ok'))))
    return True, 'live through run-sniper, no CPU option: ' + ', '.join(summary)


def case_run_sniper_qemu_system(p, g, workdir):
    """run-sniper drives full-system QEMU: --frontend=qemu-system-<arch> runs the
    machine, recording, then simulates the traces.  RISC-V: a bare-metal
    guest with an ROI (rv-bare-roi.c) simulates exactly the instructions the
    front end traces with roi=1 -- the markers reach the simulator without a
    response channel.  x86: the address-space boot image (bootas.S) ends with
    isa-debug-exit 33, which is reported, and its whole run simulates."""
    summary = []
    r, why = riscv_system_guest(workdir, 'rv-bare-roi', ['rv-bare-roi.c'],
                                ['-I', os.path.join(SNIPER_ROOT, 'include')])   # sim_api.h
    if r is not None:
        prefix = os.path.join(workdir, 'rvroi')
        rc, out = run([r['qemu'], '-M', 'virt', '-bios', 'none', '-nographic', '-kernel', r['guest'],
                       '-plugin', '%s,output=%s,roi=1' % (p['frontend'], prefix)], 300, cwd=workdir)
        m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
        if rc != 0 or not m:
            return False, 'rv-bare-roi failed under QEMU (%s)' % exit_text(rc)
        traced = int(m.group(1))
        outdir = os.path.join(workdir, 'rvroi-sim')
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '--frontend=qemu-system-riscv64', '--roi', '--',
                       '-M', 'virt', '-bios', 'none', '-nographic', '-kernel', r['guest']], 900, cwd=SNIPER_ROOT)
        if rc != 0 or 'rv roi ' + ROI_SUM not in out:
            return False, '--frontend=qemu-system-riscv64: %s' % sniper_failed(rc, out)
        n = sim_instructions(outdir)
        if abs(n - traced) > traced // 1000:
            return False, 'RISC-V: simulated %d instructions, the ROI holds %d' % (n, traced)
        summary.append('RISC-V ROI %d instructions' % n)
    outdir = os.path.join(workdir, 'bootas-sim')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'gainestown', '-d', outdir,
                   '--frontend=qemu-system-x86_64', '--',
                   '-nographic', '-display', 'none', '-accel', 'tcg', '-m', '128',
                   '-drive', 'file=%s,format=raw,if=floppy' % g['bootas.img'],
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-no-reboot'], 900, cwd=SNIPER_ROOT)
    if rc != 0:
        return False, 'run-sniper --frontend=qemu-system-x86_64 exited %s' % rc
    if 'QEMU exited with status 33' not in out:
        return False, 'x86: the guest\'s own checks did not pass (no status 33 reported)'
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    n = sim_instructions(outdir)
    if not m or abs(n - int(m.group(1))) > int(m.group(1)) // 1000:
        return False, 'x86: simulated %d instructions of the %s traced' % (n, m.group(1) if m else '?')
    summary.append('x86 boot %d instructions' % n)
    return True, '; '.join(summary) + ('' if r is not None else ' (RISC-V not run: %s)' % why)


def stats_of(outdir):
    stats = {}
    for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
        if ' = ' in line:
            k, v = line.split(' = ', 1)
            stats[k.strip()] = v.strip().split(',')[0]
    return stats


def translation_run(workdir, name, guest, config_args, extra_args=()):
    """One live run of the page-touching guest (testing/apps/touch_pages.c)
    through run-sniper --frontend=qemu; its statistics, or (None, why)."""
    outdir = os.path.join(workdir, name)
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1'] + list(config_args)
                  + ['-d', outdir, '--frontend=qemu'] + list(extra_args) + ['--', guest], 600, cwd=SNIPER_ROOT)
    if rc != 0:
        return None, (rc, out)
    return stats_of(outdir), None


TOUCH_PAGES = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'touch_pages.c')
TOUCH_FAULTS = 512 + 4 + 1      # 512 arena pages, 4 stack pages, the code page


def case_riscv_user_translation(p, g, workdir):
    """Address translation on RISC-V is Sv39: the riscv configuration builds a
    3-level table, a guest touching 512 pages in 8 regions (plus 4 of stack and
    1 of code) takes exactly that many faults, two walks each, into 12
    page-table frames; the same guest under Sv48 (--qemu-va-bits=48 with
    4_level_radix_sv48) faults the same and uses two frames more, the extra
    level's; and a guest recorded in QEMU's 48-bit layout but simulated
    against Sv39 is refused, not aliased."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    cc = os.environ['SNIPER_RISCV_CC']
    guest = os.path.join(workdir, 'touch_rv')
    rc, out = run([cc, '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d', '-O1', '-ffreestanding',
                   '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, TOUCH_PAGES], 120)
    if rc != 0:
        return None, 'could not build touch_pages.c (%s)' % exit_text(rc)
    sv39, err = translation_run(workdir, 'sv39', guest, ['-c', 'riscv'])
    if sv39 is None:
        return False, 'Sv39: %s' % sniper_failed(*err)
    if 'radix_sv39.allocated_frames' not in sv39:
        return False, 'Sv39: the riscv configuration did not build the radix_sv39 page table'
    faults, walks, frames = (int(sv39.get('mmu.page_faults', -1)), int(sv39.get('mmu.page_table_walks', -1)),
                             int(sv39['radix_sv39.allocated_frames']))
    if faults != TOUCH_FAULTS or walks != 2 * TOUCH_FAULTS or frames != 12:
        return False, 'Sv39: %d faults, %d walks, %d frames (expected %d, %d, 12)' % (faults, walks, frames, TOUCH_FAULTS, 2 * TOUCH_FAULTS) + versions_note()
    sv48, err = translation_run(workdir, 'sv48', guest, ['-c', 'riscv', '-c', 'pagetable_configs/4_level_radix_sv48'],
                                ['--qemu-va-bits=48'])
    if sv48 is None:
        return False, 'Sv48: %s' % sniper_failed(*err)
    f48, w48, fr48 = (int(sv48.get('mmu.page_faults', -1)), int(sv48.get('mmu.page_table_walks', -1)),
                      int(sv48.get('radix_sv48.allocated_frames', -1)))
    if f48 != faults or w48 != walks or fr48 != frames + 2:
        return False, 'Sv48: %d faults, %d walks, %d frames (expected %d, %d, %d: one level more, two tables)' % (f48, w48, fr48, faults, walks, frames + 2)
    bad, err = translation_run(workdir, 'mismatch', guest, ['-c', 'riscv'], ['--qemu-va-bits=48'])
    if bad is not None:
        return False, 'a guest in the 48-bit layout was simulated against Sv39 without complaint'
    rc, out = err
    if 'does not fit the 39-bit virtual address space of page table radix_sv39' not in out:
        return False, 'the 48-bit layout against Sv39 failed without the explanation: %s' % sniper_failed(rc, out)
    return True, ('Sv39: %d faults, %d walks, %d frames; Sv48: same faults, %d frames; the 48-bit layout against Sv39 refused'
                  % (faults, walks, frames, fr48))


SYSCALL_PROBE = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'syscall_probe.c')


def syscall_probe_run(workdir, guest, config_args):
    """The system-call probe (testing/apps/syscall_probe.c) live through
    run-sniper --frontend=qemu.  The simulator models a few system calls by
    their x86-64 numbers; on RISC-V and AArch64 the same numbers mean other
    calls (unlinkat is x86-64's nanosleep, mlock its clock_gettime, munlockall
    its exit_group), so the plugin must translate and filter them.  The probe
    sleeps 3 x 10 ms (the simulated time must advance by that), calls unlinkat
    on a pathname and checks the bytes are intact (exit 101 if not), then
    mlock, munlockall and dup3, which must leave the simulator running."""
    # Not workdir/probe: that is where build_guests puts the guests every
    # other case boots, and removing it here made the x86 cases that ran
    # after this one fail in 0 s with exit 1 (QEMU could not open its image).
    outdir = os.path.join(workdir, 'syscall-probe-%s' % os.path.basename(guest))
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1'] + list(config_args)
                  + ['-d', outdir, '--frontend=qemu', '--', guest], 600, cwd=SNIPER_ROOT)
    if 'syscall_probe: done' not in out:
        return False, 'the probe did not reach its end (%s): %s' % (exit_text(rc), sniper_failed(rc, out))
    if rc == 101:
        return False, "the pathname given to unlinkat was overwritten: the simulator took it for nanosleep's timespec"
    if rc not in (0, 100):
        return False, 'the probe ended with %s: %s' % (exit_text(rc), sniper_failed(rc, out))
    try:
        st = stats_of(outdir)
        t = int(st['barrier.global_time'])
    except (IOError, KeyError, ValueError) as e:
        return False, 'the run left no statistics (%s)' % e
    if t < 30 * 10**12:
        return False, 'simulated time %.1f ms after three 10 ms sleeps: nanosleep was not modelled' % (t / 1e12)
    m = re.search(r'syscall_probe: cpus=(\d+)', out)
    if not m:
        return False, 'the probe printed no CPU count'
    if int(m.group(1)) != 1:
        return False, ('sched_getaffinity gave the guest %d CPUs on a 1-core simulation: the simulator\'s '
                       'answer did not replace the call' % int(m.group(1)))
    # Time read through the system calls is simulated time (G8): the 30 ms
    # the sleeps were charged, where the host clock saw the moment they took
    m = re.search(r'syscall_probe: slept_us=(\d+) tod_us=(\d+) cputime=(\d)', out)
    if not m:
        return False, 'the probe printed no clock readings'
    slept, tod, cpu_rc = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if not (30000 <= slept <= 31000 and 30000 <= tod <= 31000):
        return False, ('clock_gettime saw %d us and gettimeofday %d us around 30 ms of simulated sleep: '
                       'the guest read the host clock' % (slept, tod))
    if cpu_rc != 0:
        return False, 'clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed'
    return True, ('nanosleep charged (%.1f ms simulated), sched_getaffinity answered by the simulator (1 CPU), '
                  'unlinkat left its pathname alone, mlock/munlockall/dup3 passed through; clock_gettime and '
                  'gettimeofday read simulated time (%d and %d us over the sleeps)' % (t / 1e12, slept, tod))


def case_riscv_user_syscalls(p, g, workdir):
    """System calls of a RISC-V guest reach the simulator translated from the
    asm-generic numbering, and only the modelled ones."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    cc = os.environ['SNIPER_RISCV_CC']
    guest = os.path.join(workdir, 'probe_rv')
    rc, out = run([cc, '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d', '-O1', '-ffreestanding',
                   '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, SYSCALL_PROBE], 120)
    if rc != 0:
        return None, 'could not build syscall_probe.c (%s)' % exit_text(rc)
    return syscall_probe_run(workdir, guest, ['-c', 'riscv'])


def case_aarch64_user_syscalls(p, g, workdir):
    """The same for an AArch64 guest (the asm-generic numbering too)."""
    qemu = os.environ.get('SNIPER_QEMU_AARCH64') or os.path.join(SNIPER_ROOT, 'qemu-aarch64', 'build', 'qemu-aarch64')
    cc = os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')
    if not os.path.isfile(qemu):
        return None, 'no qemu-aarch64 (make ARM=ON QEMU=ON, or set SNIPER_QEMU_AARCH64)'
    if not cc or not os.path.isfile(cc):
        return None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    guest = os.path.join(workdir, 'probe_a64')
    rc, out = run([cc, '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-nostdlib', '-static',
                   '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, SYSCALL_PROBE], 120)
    if rc != 0:
        return None, 'could not build syscall_probe.c (%s)' % exit_text(rc)
    return syscall_probe_run(workdir, guest, ['-c', 'cortex-a72'])


RECORD_TRACE_REFUSALS = [
    # (front end, extra options, words the refusal must contain)
    ('dr', ['--pacsim'], '--pacsim is not available with the dr front end'),
    ('dr', ['--maxthreads', '4'], '--maxthreads is not available with the dr front end'),
    ('dr', ['--roi-mpi'], '--roi-mpi is not available with the dr front end'),
    ('dr', ['--smc'], '--smc is not available with the dr front end'),
    ('dr', ['--frontend-option=-d'], '--frontend-option is not available with the dr front end'),
    ('qemu', ['--stop-address', '0x1000'], '--stop-address is not available with the qemu front end'),
    ('qemu', ['--follow'], '--follow is not available with the qemu front end'),
    ('qemu', ['--maxthreads', '4'], '--maxthreads is not available with the qemu front end'),
    ('qemu', ['--sde-arch', 'skx'], '--sde-arch is not available with the qemu front end'),
    ('qemu', ['--vma'], '--vma is not available with the qemu front end'),
    ('qemu', ['-f', '1000'], '-f/-d/-b instruction windows is not available with the qemu front end'),
    ('qemu-system-x86_64', ['--pacsim'], '--pacsim is not available with the qemu-system front end'),
    (None, ['--frontend-option=-d'], '--frontend-option is not available with the sde front end'),
    (None, ['--qemu-va-bits=48'], '--qemu-va-bits is not available with the sde front end'),
    ('drx', [], "unknown front end 'drx'"),
]


def case_record_trace_options(p, g, workdir):
    """Every front end refuses, with the reason, the options it cannot honour
    (record-trace's option table), instead of ignoring them or aborting; and
    the DynamoRIO client runs with -v, which it used to abort on."""
    rt = os.path.join(SNIPER_ROOT, 'record-trace')
    for fe, extra, want in RECORD_TRACE_REFUSALS:
        cmd = [rt, '-o', os.path.join(workdir, 'refused')] + (['--frontend=%s' % fe] if fe else []) + extra + ['--', '/bin/true']
        rc, out = run(cmd, 60, cwd=workdir)
        if rc == 0 or want not in out:
            return False, '%s %s: expected a refusal containing %r, got exit %s: %s' % (
                fe or 'sde', ' '.join(extra), want, rc, out.strip()[-300:])
    detail = '%d refusals, each with its reason' % len(RECORD_TRACE_REFUSALS)
    if os.path.exists(os.path.join(p['build'], 'lib', 'libdr-frontend.so')):
        prefix = os.path.join(workdir, 'drv')
        rc, out = run([rt, '--frontend=dr', '-v', '-o', prefix, '--', '/bin/true'], 300, cwd=workdir)
        if rc != 0 or not os.path.exists(prefix + '.app0.th0.sift') and not glob.glob(prefix + '*.sift'):
            return False, 'the DynamoRIO client with -v: exit %s: %s' % (rc, out.strip()[-300:])
        detail += '; the DynamoRIO client runs with -v'
    return True, detail


def case_riscv_virtuoso_portable(p, g, workdir):
    """Virtuoso on RISC-V: the allocator and page-table fragments layer on the
    riscv configuration (a THP allocator maps the page-touching guest with
    2 MiB pages: 10 faults, not 517; a hashed page table replaces Sv39),
    while an address_translation_schemes/ file -- a complete x86 system on
    the Meteor Lake core -- is refused with the reason in either order."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    cc = os.environ['SNIPER_RISCV_CC']
    guest = os.path.join(workdir, 'touch_rv_v')
    rc, out = run([cc, '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d', '-O1', '-ffreestanding',
                   '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, TOUCH_PAGES], 120)
    if rc != 0:
        return None, 'could not build touch_pages.c (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'touchv')
    rc, out = run([r['qemu']] + RISCV_VA_RESERVE + ['-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 120, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (%s)' % exit_text(rc)
    trace = '--traces=' + prefix + '.app0.th0.sift'
    def sim(name, configs, extra=()):
        outdir = os.path.join(workdir, 'virt-' + name)
        shutil.rmtree(outdir, ignore_errors=True)
        args = []
        for c in configs:
            args += ['-c', c]
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1'] + args + list(extra) + ['-d', outdir, trace],
                      600, cwd=SNIPER_ROOT)
        return rc, out, outdir
    # reserve_thp ships with every 2 MiB page fragmented (target_fragmentation
    # 0.0); with them all free it maps each touched region with one.
    rc, out, d = sim('thp', ['riscv', 'physical_memory_allocators/reserve_thp'],
                     ['-g', 'perf_model/reserve_thp/target_fragmentation=1.0'])
    if rc != 0:
        return False, 'riscv + reserve_thp: %s' % sniper_failed(rc, out)
    faults = int(stats_of(d).get('mmu.page_faults', -1))
    if faults != 10:
        return False, 'riscv + reserve_thp: %d page faults, expected 10 (8 regions, stack and code in 2 MiB pages)' % faults
    rc, out, d = sim('hash', ['riscv', 'pagetable_configs/hash_table_chaining'])
    if rc != 0:
        return False, 'riscv + hash_table_chaining: %s' % sniper_failed(rc, out)
    st = stats_of(d)
    if not any(k.startswith('hash_table_chaining.') for k in st) or any(k.startswith('radix_sv39.') for k in st):
        return False, 'riscv + hash_table_chaining did not replace the Sv39 table'
    if int(st.get('mmu.page_faults', -1)) != TOUCH_FAULTS:
        return False, 'riscv + hash_table_chaining: %s page faults, expected %d' % (st.get('mmu.page_faults'), TOUCH_FAULTS) + versions_note()
    for order in (['address_translation_schemes/baseline', 'riscv'], ['riscv', 'address_translation_schemes/baseline']):
        rc, out, d = sim('mixed', order)
        if rc == 0 or 'mixes a core built for intel' not in out:
            return False, '%s was not refused with the reason (exit %s)' % (' + '.join(order), rc)
    return True, 'reserve_thp with free 2 MiB pages: 10 faults; hash_table_chaining replaces Sv39; a scheme with riscv refused in either order'


PHASED_MT = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'phased_rv_mt.c')
PHASED_MT_LINE = 'phased_rv_mt: threads=4 phases=12 checksum=12291709628096540252 visited=1440000'
MT_REFUSAL = 'The front end refused the run: [SNIPER-QEMU] error: the guest started a second thread.'


def qemu_can_wait(qemu):
    """Does this QEMU have the plugin wait call (tools/qemu-11.1.1-plugin-wait.patch)?
    With it the plugin runs multi-threaded guests live; without it, it refuses
    the second thread."""
    try:
        with open(qemu, 'rb') as f:
            return b'qemu_plugin_vcpu_wait_at' in f.read()
    except IOError:
        return False


COUNTER_PROBE = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'counter_probe.c')
TIMED_RAW = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'timed_raw.c')
TIMED_WAITS = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'timed_waits.c')
TIME_PROBE = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'time_probe.c')
# Sniper's osemu/time_start default, which the front ends share
# (Sift::TIME_START_DEFAULT, sift/sift_format.h)
TIME_START_DEFAULT = 1337000000


def qemu_has_counters(qemu):
    """Does this QEMU let the plugin answer the guest's counter reads
    (tools/qemu-11.1.1-plugin-counters.patch)?"""
    try:
        with open(qemu, 'rb') as f:
            return b'qemu_plugin_register_counter_cb' in f.read()
    except IOError:
        return False


def qemu_can_run_syscall(qemu):
    """Does this QEMU let the plugin run a system call of its own
    (tools/qemu-11.1.1-plugin-syscall.patch)?  Without it the guest's
    timeouts are handed to the host's kernel."""
    try:
        with open(qemu, 'rb') as f:
            return b'qemu_plugin_run_syscall' in f.read()
    except IOError:
        return False


def timed_lines(out, prefix):
    """{name: (want, got, ret)} from a timed-wait guest's lines"""
    return {m.group(1): (int(m.group(2)), int(m.group(3)), int(m.group(4)))
            for m in re.finditer(prefix + r': (\S+) want=(\d+) got=(\d+) ret=(-?\d+)', out)}


def timed_check(what, lines, expect):
    """Every wait ended at its deadline in the guest's time (got: want, and at
    most 1% and 100 us more), the ones another thread ends (want 0) before
    their 1 s timeout; and each wait the guest runs reported"""
    for name in expect:
        if name not in lines:
            return '%s: no line for %s' % (what, name)
        want, got, ret = lines[name]
        if want and not want <= got <= want * 1.01 + 100000:
            return '%s: %s ended after %d ns of the guest\'s time, not its %d' % (what, name, got, want)
        if not want and got >= 1000000000:
            return '%s: %s ran to its timeout (%d ns) instead of being woken' % (what, name, got)
    return None


TIMED_RAW_WAITS = ('clock_nanosleep-abs', 'ppoll', 'pselect6', 'epoll_pwait', 'futex-wait',
                   'futex-bitset-realtime', 'rt_sigtimedwait', 'ppoll-2s')


def timed_case(workdir, arch, cc_cmd, config):
    """The guest's timeouts in the guest's time (testing/apps/timed_raw.c,
    raw system calls): every timed wait ends at its deadline in the guest's
    clock, the one-IPC model's in a trace written to a file, the simulator's
    live -- never the host's kernel's, which timed them in host seconds (or
    took a deadline from the guest's clock, 14 years off the host's, as one
    of its own).  The 2 s wait takes 2 s of no clock but the guest's: the
    recording to a file finishes in less (it took 2 s more before)."""
    qemu = user_qemu(arch)
    if not os.path.isfile(qemu):
        return None, 'no qemu-%s' % arch
    if not qemu_can_run_syscall(qemu):
        return None, 'this QEMU has no plugin system call (tools/qemu-11.1.1-plugin-syscall.patch): the guest\'s timeouts run in host time'
    guest = os.path.join(workdir, 'timed_raw_' + arch)
    rc, out = run(cc_cmd + ['-O1', '-ffreestanding', '-nostdlib', '-static', '-Wl,-e,_start', '-o', guest, TIMED_RAW], 120)
    if rc != 0:
        return None, 'could not build timed_raw.c (%s)' % exit_text(rc)
    seen = []
    t0 = time.time()
    rc, out = run([os.path.join(SNIPER_ROOT, 'record-trace'), '--frontend=qemu', '-o', os.path.join(workdir, 'timed-' + arch),
                   '--', guest], 300, cwd=workdir)
    host = time.time() - t0
    if rc != 0 or 'timed_raw: done ok' not in out:
        return False, 'to a file: the probe failed (%s): %s' % (exit_text(rc), out.strip()[-400:])
    why = timed_check('to a file', timed_lines(out, 'timed_raw'), TIMED_RAW_WAITS)
    if why:
        return False, why
    if host >= 2.0:
        return False, 'to a file: the recording took %.1f s of the host\'s time: the 2 s wait was timed by the host' % host
    seen.append('to a file every wait at its deadline (%.1f s of host time)' % host)
    outdir = os.path.join(workdir, 'timed-live-' + arch)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', config, '-d', outdir,
                   '--frontend=qemu', '--', guest], 900, cwd=SNIPER_ROOT)
    if rc != 0 or 'timed_raw: done ok' not in out:
        return False, 'live: the probe failed (%s): %s' % (exit_text(rc), sniper_failed(rc, out))
    why = timed_check('live', timed_lines(out, 'timed_raw'), TIMED_RAW_WAITS)
    if why:
        return False, why
    seen.append('live every wait at its deadline in simulated time')
    return True, '; '.join(seen)


def case_user_timed(p, g, workdir):
    """x86-64 timed waits in the guest's time (timed_case)."""
    return timed_case(workdir, 'x86_64', ['gcc'], 'gainestown')


def case_riscv_user_timed(p, g, workdir):
    """RISC-V timed waits in the guest's time (timed_case)."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    return timed_case(workdir, 'riscv64', [os.environ['SNIPER_RISCV_CC'], '--target=riscv64-linux-gnu',
                      '-march=rv64gc', '-mabi=lp64d', '-fuse-ld=lld'], 'riscv')


def case_aarch64_user_timed(p, g, workdir):
    """AArch64 timed waits in the guest's time (timed_case)."""
    qemu, cc, why = aarch64_tools()
    if why:
        return None, why
    return timed_case(workdir, 'aarch64', [cc, '--target=aarch64-linux-gnu', '-fuse-ld=lld'], 'cortex-a72')


TIMED_WAITS_ALL = ('nanosleep', 'clock_nanosleep-abs', 'poll', 'ppoll', 'select', 'epoll_wait', 'sigtimedwait',
                   'cond_timedwait-realtime', 'sem_timedwait', 'poll-2s', 'poll-ready', 'sem_timedwait-posted')


def case_user_timed_libc(p, g, workdir):
    """The libc's timed waits on x86-64 (testing/apps/timed_waits.c: nanosleep,
    poll, select, epoll, sigtimedwait, a condition variable's and a
    semaphore's timed waits, a 2 s poll) at their deadlines in the guest's
    time, to a file and live; a poll and a semaphore wait that another thread
    ends before their 1 s timeouts end then."""
    qemu = user_qemu('x86_64')
    if not os.path.isfile(qemu):
        return None, 'no qemu-x86_64'
    if not qemu_can_run_syscall(qemu):
        return None, 'this QEMU has no plugin system call (tools/qemu-11.1.1-plugin-syscall.patch)'
    guest = os.path.join(workdir, 'timed_waits')
    rc, out = run(['gcc', '-O1', '-static', '-pthread', '-o', guest, TIMED_WAITS], 120)
    if rc != 0:
        return None, 'could not build timed_waits.c statically (%s)' % exit_text(rc)
    rc, out = run([os.path.join(SNIPER_ROOT, 'record-trace'), '--frontend=qemu', '-o', os.path.join(workdir, 'timed-libc'),
                   '--', guest], 300, cwd=workdir)
    if rc != 0 or 'timed_waits: done' not in out:
        return False, 'to a file: the guest failed (%s): %s' % (exit_text(rc), out.strip()[-400:])
    why = timed_check('to a file', timed_lines(out, 'timed_waits'), TIMED_WAITS_ALL)
    if why:
        return False, why
    # Live, the waits another thread ends are left out for now: a thread that
    # sleeps in the simulator while another runs is not woken (the sleep fix)
    live = [w for w in TIMED_WAITS_ALL if w not in ('poll-ready', 'sem_timedwait-posted')]
    outdir = os.path.join(workdir, 'timed-libc-live')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '2', '-c', 'gainestown', '-d', outdir,
                   '--frontend=qemu', '--', guest], 900, cwd=SNIPER_ROOT,
                  env=dict(os.environ, TIMED_WAITS_ONLY=','.join(live)))
    if rc != 0 or 'timed_waits: done' not in out:
        return False, 'live: the guest failed (%s): %s' % (exit_text(rc), sniper_failed(rc, out))
    why = timed_check('live', timed_lines(out, 'timed_waits'), live)
    if why:
        return False, why
    return True, 'to a file %d waits, live %d, each at its deadline in the guest\'s time' % (len(TIMED_WAITS_ALL), len(live))


def counters_case(workdir, arch, cc_cmd, config):
    """The counter probe (testing/apps/counter_probe.c) live through run-sniper
    --frontend=qemu: the guest's direct reads of its clocks and counters --
    RISC-V time, cycle and instret, AArch64 CNTVCT, x86's TSC -- see the
    simulated time, as clock_gettime() does.  Around three 10 ms sleeps the
    simulator charges 30 ms while the host barely sleeps; each counter must
    show the same 30 ms at its rate: the timer at its frequency (CNTFRQ, or
    the RISC-V timebase the plugin uses, 10 MHz), the cycle counters at the
    configuration's core frequency.  They read the host's clock before
    (tools/qemu-11.1.1-plugin-counters.patch)."""
    qemu = user_qemu(arch)
    if not os.path.isfile(qemu):
        return None, 'no qemu-%s' % arch
    if not qemu_has_counters(qemu):
        return None, 'this QEMU has no counter callback (tools/qemu-11.1.1-plugin-counters.patch): the guest reads the host\'s counters'
    guest = os.path.join(workdir, 'counter_probe_' + arch)
    rc, out = run(cc_cmd + ['-O1', '-ffreestanding', '-nostdlib', '-static', '-Wl,-e,_start', '-o', guest, COUNTER_PROBE], 120)
    if rc != 0:
        return None, 'could not build counter_probe.c (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'counters-' + arch)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', config, '-d', outdir,
                   '--frontend=qemu', '--', guest], 600, cwd=SNIPER_ROOT)
    m = re.search(r'counter_probe: start=(\d+) ns=(\d+) time=(\d+) freq=(\d+) cycles=(\d+) instret=(\d+) loop_instret=(\d+)', out)
    if rc != 0 or not m:
        return False, 'the probe did not run (%s): %s' % (exit_text(rc), sniper_failed(rc, out))
    start, ns, time_, freq, cycles, instret, loop = (int(x) for x in m.groups())
    if start != TIME_START_DEFAULT:
        return False, 'CLOCK_REALTIME started at %d s, not at the simulator\'s time zero %d' % (start, TIME_START_DEFAULT)
    if not 30000000 <= ns <= 31000000:
        return False, 'clock_gettime saw %d ns over 30 ms of simulated sleep' % ns
    try:
        cfg = open(os.path.join(outdir, 'simulation', 'sim.cfg')).read()
        ghz = float(re.search(r'\[perf_model/core\][^\[]*?\nfrequency\s*=\s*"?([0-9.]+)', cfg).group(1))
    except (IOError, AttributeError) as e:
        return False, 'no core frequency in the run\'s sim.cfg (%s)' % e
    def near(value, want, what):
        if not abs(value - want) <= 0.01 * want + 2:
            return '%s %d, expected %.0f (the %d ns the clock saw)' % (what, value, want, ns)
        return None
    seen = []
    if arch != 'x86_64':
        rate = freq or 10000000                       # CNTFRQ, or the plugin's RISC-V timebase
        why = near(time_, ns * rate / 1e9, 'the timer moved')
        if why:
            return False, why + '; it reads the host\'s clock' if time_ < ns * rate / 1e9 / 2 else why
        seen.append('timer %d at %d Hz' % (time_, rate))
    if arch != 'aarch64':
        why = near(cycles, ns * ghz, 'the cycle counter moved')
        if why:
            return False, why
        seen.append('cycles %d at %g GHz' % (cycles, ghz))
    if arch == 'riscv64':
        if not 100000 * 2 <= loop <= 100000 * 12 or instret > loop:
            return False, 'instret: %d over a 100,000-iteration loop, %d over the sleeps' % (loop, instret)
        seen.append('instret %d over the loop' % loop)
    return True, '30 ms of simulated sleep (%d ns): %s' % (ns, ', '.join(seen))


def case_time_start_default(p, g, workdir):
    """The guest's time recorded to a file, with no simulator to answer it:
    every front end gives the one-IPC model (one instruction per cycle at an
    assumed 1 GHz, from the thread's start) from Sniper's time zero
    (osemu/time_start's default, 1337000000 s), never the host's clock --
    the same for the SIFT recorder (SDE) and the QEMU plugin, and the same
    every time.  The libc probe (testing/apps/time_probe.c: clock_gettime,
    gettimeofday, the TSC around a 100,000-iteration loop) under SDE and
    QEMU x86-64; the counter probe (raw system calls, the timer, cycle and
    instret counters) under QEMU on RISC-V and AArch64, where the plugin
    answers the system calls itself."""
    rt = os.path.join(SNIPER_ROOT, 'record-trace')
    seen = []
    # x86-64 through libc, under the SIFT recorder and under QEMU
    guest = os.path.join(workdir, 'time_probe')
    rc, out = run(['gcc', '-O1', '-static', '-o', guest, TIME_PROBE], 120)
    if rc != 0:
        return None, 'could not build time_probe.c statically (%s)' % exit_text(rc)
    rx = r'time_probe: realtime=(\d+)\.(\d+) mono=(\d+)\.(\d+) tod=(\d+)\.(\d+) dreal=(-?\d+) dtsc=(\d+)'
    fes = [('sde', [])]
    if os.path.isfile(user_qemu('x86_64')) and qemu_has_counters(user_qemu('x86_64')):
        fes.append(('qemu', ['--frontend=qemu']))
    readings = {}
    for fe, opt in fes:
        lines = []
        for k in (1, 2):
            rc, out = run([rt] + opt + ['-o', os.path.join(workdir, 'tp-%s-%d' % (fe, k)), '--', guest], 300, cwd=workdir)
            m = re.search(rx, out)
            if rc != 0 or not m:
                return False, '%s: the libc probe did not run (%s): %s' % (fe, exit_text(rc), out.strip()[-300:])
            lines.append(m.group(0))
        # The model is deterministic given the instructions run, but a program's
        # start-up runs a few more or fewer from one run to the next (its
        # address layout, its environment): the first readings agree to a
        # microsecond, the intervals exactly
        v, w = ([int(x) for x in re.search(rx, l).groups()] for l in lines)
        first_v, first_w = v[0] * 10**9 + v[1], w[0] * 10**9 + w[1]
        if abs(first_v - first_w) > 1000 or v[6:] != w[6:]:
            return False, '%s: two recordings read different times: %s / %s' % (fe, lines[0], lines[1])
        if v[0] != TIME_START_DEFAULT or v[2] != TIME_START_DEFAULT or v[4] != TIME_START_DEFAULT:
            return False, '%s: the clocks start at %d, %d and %d s, not at the time zero %d' % (fe, v[0], v[2], v[4], TIME_START_DEFAULT)
        dreal, dtsc = v[6], v[7]
        if not 100000 <= dreal <= 2000000 or not abs(dtsc - dreal) <= 0.05 * dreal:
            return False, '%s: %d ns and %d TSC cycles over a 100,000-iteration loop: not one instruction a nanosecond and a cycle' % (fe, dreal, dtsc)
        readings[fe] = v
        seen.append('%s %d.%09d s, %d ns and %d cycles over the loop' % (fe, v[0], v[1], dreal, dtsc))
    # x86-64 raw system calls and rdtsc under the SIFT recorder; RISC-V and
    # AArch64 through the counter probe under QEMU
    g_ = os.path.join(workdir, 'counter_probe_off_sde')
    rc, out = run(['gcc', '-O1', '-ffreestanding', '-nostdlib', '-static', '-Wl,-e,_start', '-o', g_, COUNTER_PROBE], 120)
    if rc != 0:
        return None, 'could not build counter_probe.c for x86-64 (%s)' % exit_text(rc)
    lines = []
    for k in (1, 2):
        rc, out = run([rt, '-o', os.path.join(workdir, 'cp-sde-%d' % k), '--', g_], 300, cwd=workdir)
        m = re.search(r'counter_probe: start=(\d+) ns=(\d+) time=(\d+) freq=(\d+) cycles=(\d+) instret=(\d+) loop_instret=(\d+)', out)
        if not m:
            return False, 'sde: the counter probe did not run (%s): %s' % (exit_text(rc), out.strip()[-300:])
        lines.append(m.group(0))
    if lines[0].split(' ns=')[1] != lines[1].split(' ns=')[1] or lines[0].split(' ns=')[0] != lines[1].split(' ns=')[0]:
        return False, 'sde: two recordings read different counters: %s / %s' % (lines[0], lines[1])
    start, ns, cycles = (int(x) for x in re.search(r'start=(\d+) ns=(\d+) time=\d+ freq=\d+ cycles=(\d+)', lines[0]).groups())
    if start != TIME_START_DEFAULT or not 0 < ns < 10000 or abs(cycles - ns) > 0.05 * ns + 2:
        return False, 'sde, raw system calls: start %d s, %d ns and %d TSC cycles over the sleeps' % (start, ns, cycles)
    seen.append('sde raw calls from %d s, %d ns and %d cycles over the sleeps' % (start, ns, cycles))
    for arch, cc_cmd in (('riscv64', [os.environ.get('SNIPER_RISCV_CC', ''), '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d', '-fuse-ld=lld']),
                         ('aarch64', [os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC', ''), '--target=aarch64-linux-gnu', '-fuse-ld=lld'])):
        qemu = user_qemu(arch)
        if not cc_cmd[0] or not os.path.isfile(cc_cmd[0]) or not os.path.isfile(qemu) or not qemu_has_counters(qemu):
            seen.append('%s skipped (no compiler or no QEMU with the counter callback)' % arch)
            continue
        g_ = os.path.join(workdir, 'counter_probe_off_' + arch)
        rc, out = run(cc_cmd + ['-O1', '-ffreestanding', '-nostdlib', '-static', '-Wl,-e,_start', '-o', g_, COUNTER_PROBE], 120)
        if rc != 0:
            return None, 'could not build counter_probe.c for %s (%s)' % (arch, exit_text(rc))
        lines = []
        for k in (1, 2):
            rc, out = run([rt, '--frontend=qemu', '-o', os.path.join(workdir, 'cp-%s-%d' % (arch, k)), '--', g_], 300, cwd=workdir)
            m = re.search(r'counter_probe: start=(\d+) ns=(\d+) time=(\d+) freq=(\d+) cycles=(\d+) instret=(\d+) loop_instret=(\d+)', out)
            if not m:
                return False, '%s: the counter probe did not run (%s): %s' % (arch, exit_text(rc), out.strip()[-300:])
            lines.append(m.group(0))
        if lines[0] != lines[1]:   # intervals and the start second: exact
            return False, '%s: two recordings read different counters: %s / %s' % (arch, lines[0], lines[1])
        start, ns, time_, freq, cycles, instret, loop = (int(x) for x in re.search(r'start=(\d+) ns=(\d+) time=(\d+) freq=(\d+) cycles=(\d+) instret=(\d+) loop_instret=(\d+)', lines[0]).groups())
        if start != TIME_START_DEFAULT:
            return False, '%s: CLOCK_REALTIME started at %d s, not at the time zero %d' % (arch, start, TIME_START_DEFAULT)
        # the sleeps run no instructions: the one-IPC clock moves by the few between the reads
        if not 0 < ns < 10000:
            return False, '%s: %d ns over the sleeps; the one-IPC clock moves by the instructions run' % (arch, ns)
        rate = freq or 10000000
        if abs(time_ - ns * rate / 1e9) > 0.05 * ns * rate / 1e9 + 2:
            return False, '%s: the timer moved %d at %d Hz for %d ns' % (arch, time_, rate, ns)
        if arch == 'riscv64' and (abs(cycles - instret) > 2 or not 100000 * 2 <= loop <= 100000 * 12):
            return False, '%s: cycles %d, instret %d over the sleeps, %d over the loop' % (arch, cycles, instret, loop)
        seen.append('%s from %d s, %d ns and timer %d over the sleeps' % (arch, start, ns, time_))
    return True, '; '.join(seen)


def case_user_counters(p, g, workdir):
    """The guest's TSC (rdtsc) under x86-64 user mode reads simulated time."""
    return counters_case(workdir, 'x86_64', ['gcc'], 'gainestown')


def case_riscv_user_counters(p, g, workdir):
    """RISC-V time, cycle and instret under user mode read simulated time."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    return counters_case(workdir, 'riscv64', [os.environ['SNIPER_RISCV_CC'], '--target=riscv64-linux-gnu',
                         '-march=rv64gc', '-mabi=lp64d', '-fuse-ld=lld'], 'riscv')


def case_aarch64_user_counters(p, g, workdir):
    """AArch64 CNTVCT_EL0 under user mode reads simulated time."""
    qemu, cc, why = aarch64_tools()
    if why:
        return None, why
    return counters_case(workdir, 'aarch64', [cc, '--target=aarch64-linux-gnu', '-fuse-ld=lld'], 'cortex-a72')


def mt_live_check(rc, out, line, qemu):
    """A multi-threaded guest run live through run-sniper: with the wait call
    in the QEMU it runs to the end with the native checksum; without it the
    plugin refuses the second thread with its reason and the run fails
    instead of passing on the first thread's partial simulation.  Returns
    (ok, what happened)."""
    if qemu_can_wait(qemu):
        if rc != 0:
            return False, 'live: %s' % sniper_failed(rc, out)
        if line not in out:
            return False, 'live: the run passed with a wrong or no checksum'
        return True, 'live: ran with the native checksum'
    if rc == 0:
        return False, 'live: a QEMU without the wait call ran a multi-threaded guest; the plugin must refuse the second thread'
    if MT_REFUSAL not in out:
        return False, 'live: the run failed without the refusal (%s): %s' % (exit_text(rc), sniper_failed(rc, out))
    # The refusal ends QEMU with _exit(1); a crash on the way out (QEMU
    # died of SIGSEGV in one refusal in four while exit handlers ran under
    # the other vCPU) reaches run-sniper as status 139.
    if rc != 1:
        return False, 'live: the refusal was given but the run ended with %s, not exit 1' % exit_text(rc)
    return True, 'live: this QEMU has no wait call, the second thread refused and the run failed (exit %s)' % rc


def user_qemu(arch):
    var, path = {'x86_64': ('SNIPER_QEMU_X86_64', 'qemu/build/qemu-x86_64'),
                 'riscv64': ('SNIPER_QEMU_RISCV', 'qemu-riscv/build/qemu-riscv64'),
                 'aarch64': ('SNIPER_QEMU_AARCH64', 'qemu-aarch64/build/qemu-aarch64')}[arch]
    return os.environ.get(var) or os.path.join(SNIPER_ROOT, path)


def mt_case(p, workdir, arch, qemu, cc, target_flags, config, qemu_opts):
    """The four-worker phased guest under QEMU user mode: recorded to files it
    gives one readable trace per thread (main and four workers) and the same
    checksum as native; run live through run-sniper, as mt_live_check says."""
    guest = os.path.join(workdir, 'phased_mt_' + arch)
    rc, out = run([cc] + target_flags + ['-O1', '-ffreestanding', '-nostdlib', '-static', '-fuse-ld=lld',
                   '-Wl,-e,_start', '-I', os.path.join(SNIPER_ROOT, 'include'), '-DNTHREADS=4', '-o', guest, PHASED_MT], 120)
    if rc != 0:
        return None, 'could not build phased_rv_mt.c (%s)' % exit_text(rc)
    prefix = os.path.join(workdir, 'mt_' + arch)
    rc, out = run([qemu] + qemu_opts + ['-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    if rc != 0 or PHASED_MT_LINE not in out:
        return False, 'recording: exit %s, %s' % (exit_text(rc), 'checksum as native' if PHASED_MT_LINE in out else 'wrong or no checksum')
    traces = sorted(f for f in os.listdir(workdir) if f.startswith('mt_%s.app0.th' % arch) and f.endswith('.sift'))
    if len(traces) != 5:
        return False, '%d per-thread traces, expected 5 (main and four workers)' % len(traces)
    for t in traces:
        rc, _ = run([p['siftdump'], os.path.join(workdir, t)], 300)
        if rc != 0:
            return False, '%s unreadable (%s)' % (t, exit_text(rc))
    outdir = os.path.join(workdir, 'mt-live-' + arch)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '4', '-c', config, '-d', outdir,
                   '--frontend=qemu', '--', guest], 1200, cwd=SNIPER_ROOT)
    ok, live = mt_live_check(rc, out, PHASED_MT_LINE, user_qemu({'rv': 'riscv64', 'a64': 'aarch64'}[arch]))
    if not ok:
        return False, live
    return True, '5 per-thread traces, checksum as native; ' + live


# phased.c, small: four workers on pthread barriers, the ROI around the phases
PHASED_X86_FLAGS = ['-DNTHREADS=4', '-DNPHASES=6', '-DCHASE_SLOTS=65536', '-DCHASE_STEPS=20000', '-DCOMPUTE_ITERS=40000']


def case_user_mt_live(p, g, workdir):
    """A multi-threaded x86-64 guest (pthreads, four workers meeting at
    barriers) run live through run-sniper --roi: with the plugin wait call in
    the QEMU it runs on five cores -- futex calls answered by the simulator,
    threads announced at their clone's return, joined through the simulator's
    model of their tid words -- and prints the native checksum; without the
    call, the second thread is refused with the reason."""
    guest = os.path.join(workdir, 'phased_x86')
    rc, out = run(['gcc', '-O1', '-static', '-I', os.path.join(SNIPER_ROOT, 'include')] + PHASED_X86_FLAGS +
                  ['-o', guest, os.path.join(SNIPER_ROOT, 'testing', 'apps', 'phased.c'), '-lpthread'], 120)
    if rc != 0:
        return None, 'could not build phased.c statically (%s)' % exit_text(rc)
    rc, out = run([guest], 120)
    m = re.search(r'phased: .*checksum=\S+ visited=\d+', out)
    if rc != 0 or not m:
        return None, 'the guest did not run natively (%s)' % exit_text(rc)
    line = m.group(0)
    outdir = os.path.join(workdir, 'mt-live-x86')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '5', '-c', 'gainestown', '--roi', '-d', outdir,
                   '--frontend=qemu', '--', guest], 1200, cwd=SNIPER_ROOT)
    return mt_live_check(rc, out, line, user_qemu('x86_64'))


def case_user_roi_wait(p, g, workdir):
    """A worker waiting on a condition variable when the ROI ends, woken after
    it (testing/apps/roi_wait.c), run live on x86-64: the futex calls stay in
    the simulator's model after the ROI -- the wake, and the exiting worker's
    wake of its joiner -- or the waiter is never woken (it hung every time).
    Without the wait call in QEMU the second thread is refused."""
    guest = os.path.join(workdir, 'roi_wait')
    rc, out = run(['gcc', '-O1', '-static', '-pthread', '-I', os.path.join(SNIPER_ROOT, 'include'),
                   '-o', guest, os.path.join(SNIPER_ROOT, 'testing', 'apps', 'roi_wait.c')], 120)
    if rc != 0:
        return None, 'could not build roi_wait.c statically (%s)' % exit_text(rc)
    rc, out = run([guest], 120)
    m = re.search(r'roi_wait: done$', out, re.M)
    if rc != 0 or not m:
        return None, 'the guest did not run natively (%s)' % exit_text(rc)
    outdir = os.path.join(workdir, 'roi-wait-x86')
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '3', '-c', 'gainestown', '--roi', '-d', outdir,
                   '--frontend=qemu', '--', guest], 600, cwd=SNIPER_ROOT)
    return mt_live_check(rc, out, m.group(0), user_qemu('x86_64'))


def post_roi_case(workdir, name, ncores, rx, check):
    """A static x86-64 guest run live through run-sniper --frontend=qemu --roi,
    with what it does after SimRoiEnd() checked: a live run stays live after
    the ROI (counting, every call the simulator's), so its clock goes on and
    its threads and timed waits are the simulator's."""
    guest = os.path.join(workdir, name)
    rc, out = run(['gcc', '-O1', '-static', '-pthread', '-I', os.path.join(SNIPER_ROOT, 'include'),
                   '-o', guest, os.path.join(SNIPER_ROOT, 'testing', 'apps', name + '.c')], 120)
    if rc != 0:
        return None, 'could not build %s.c statically (%s)' % (name, exit_text(rc))
    outdir = os.path.join(workdir, 'post-roi-' + name)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', str(ncores), '-c', 'gainestown', '--roi', '-d', outdir,
                   '--frontend=qemu', '--', guest], 600, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'the run hung after the ROI (timed out)'
    m = re.search(rx, out)
    if rc != 0 or not m:
        return False, 'the guest failed after the ROI (%s): %s' % (exit_text(rc), sniper_failed(rc, out))
    return check(m)


def case_user_roi_time(p, g, workdir):
    """After the ROI the guest's clock goes on: testing/apps/roi_time.c spins on
    clock_gettime until 1 ms has passed and the TSC moved (it froze before:
    nothing was counted to the simulator after SimRoiEnd())."""
    def check(m):
        ns, reads, moved = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if ns < 1000000 or not moved:
            return False, 'after the ROI the clock moved %d ns in %d reads, the TSC %s' % (ns, reads, 'moved' if moved else 'stood still')
        return True, 'after the ROI the clock reached %d ns in %d reads and the TSC moved' % (ns, reads)
    return post_roi_case(workdir, 'roi_time', 2, r'roi_time: elapsed_ns=(\d+) reads=(\d+) tsc_moved=(\d)', check)


def case_user_roi_threads(p, g, workdir):
    """After the ROI, threads, joins and timed waits stay the simulator's:
    testing/apps/roi_threads.c starts a thread that starts another, joins
    both, and times out a condition wait at an absolute deadline 10 ms
    ahead, in simulated time.  Needs the plugin wait call in QEMU."""
    if not qemu_can_wait(user_qemu('x86_64')):
        return None, 'this QEMU has no plugin wait call: a second thread is refused live'
    def check(m):
        joined, how, waited = int(m.group(1)), m.group(2), int(m.group(3))
        if joined != 2 or how != 'ETIMEDOUT' or not 10000000 <= waited <= 11000000:
            return False, 'after the ROI: %d threads joined, the timed wait %s after %d ns' % (joined, how, waited)
        return True, 'after the ROI: 2 threads started and joined, the 10 ms timed wait timed out at %d ns of simulated time' % waited
    return post_roi_case(workdir, 'roi_threads', 4, r'roi_threads: joined=(\d+) timedwait=(\w+) waited_ns=(-?\d+)', check)


def case_riscv_user_mt(p, g, workdir):
    """Multi-threaded RISC-V guest: traces per thread, and the live refusal."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    return mt_case(p, workdir, 'rv', r['qemu'], os.environ['SNIPER_RISCV_CC'],
                   ['--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d'], 'riscv', RISCV_VA_RESERVE)


def case_aarch64_user_mt(p, g, workdir):
    """The same for an AArch64 guest."""
    qemu = os.environ.get('SNIPER_QEMU_AARCH64') or os.path.join(SNIPER_ROOT, 'qemu-aarch64', 'build', 'qemu-aarch64')
    cc = os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')
    if not os.path.isfile(qemu):
        return None, 'no qemu-aarch64 (make ARM=ON QEMU=ON, or set SNIPER_QEMU_AARCH64)'
    if not cc or not os.path.isfile(cc):
        return None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    return mt_case(p, workdir, 'a64', qemu, cc, ['--target=aarch64-linux-gnu'], 'cortex-a72', [])


def case_aarch64_user_translation(p, g, workdir):
    """Address translation on AArch64 is the 4 KiB-granule 4-level table (the
    cortex configurations' radix_4level): the page-touching guest takes one
    fault per page it touches, two walks each, into 14 page-table frames."""
    qemu = os.environ.get('SNIPER_QEMU_AARCH64') or os.path.join(SNIPER_ROOT, 'qemu-aarch64', 'build', 'qemu-aarch64')
    cc = os.environ.get('SNIPER_AARCH64_CC') or os.environ.get('SNIPER_RISCV_CC')
    if not os.path.isfile(qemu):
        return None, 'no qemu-aarch64 (make ARM=ON QEMU=ON, or set SNIPER_QEMU_AARCH64)'
    if not cc or not os.path.isfile(cc):
        return None, 'no clang for aarch64 (set SNIPER_AARCH64_CC, or SNIPER_RISCV_CC to a clang)'
    guest = os.path.join(workdir, 'touch_a64')
    rc, out = run([cc, '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-nostdlib', '-static',
                   '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest, TOUCH_PAGES], 120)
    if rc != 0:
        return None, 'could not build touch_pages.c (%s)' % exit_text(rc)
    st, err = translation_run(workdir, 'a64', guest, ['-c', 'cortex-a72'])
    if st is None:
        return False, sniper_failed(*err)
    if 'radix_4level.allocated_frames' not in st:
        return False, 'the cortex-a72 configuration did not build the radix_4level page table'
    faults, walks, frames = (int(st.get('mmu.page_faults', -1)), int(st.get('mmu.page_table_walks', -1)),
                             int(st['radix_4level.allocated_frames']))
    # one data fault more than on RISC-V: this build's start-up touches one more stack page
    if faults != TOUCH_FAULTS + 1 or walks != 2 * faults or frames != 14:
        return False, '%d faults, %d walks, %d frames (expected %d, %d, 14)' % (faults, walks, frames, TOUCH_FAULTS + 1, 2 * (TOUCH_FAULTS + 1)) + versions_note(qemu)
    return True, '4-level table: %d faults, %d walks, %d frames' % (faults, walks, frames)

CASES = {
    'user-trace':       case_user_trace,
    'user-simulate':    case_user_simulate,
    'user-mt-streams':  case_user_mt_streams,
    'user-mt-live':     case_user_mt_live,
    'user-roi-wait':    case_user_roi_wait,
    'user-roi-time':    case_user_roi_time,
    'user-roi-threads': case_user_roi_threads,
    'user-timed':       case_user_timed,
    'user-timed-libc':  case_user_timed_libc,
    'riscv-user-timed': case_riscv_user_timed,
    'aarch64-user-timed': case_aarch64_user_timed,
    'user-counters':    case_user_counters,
    'time-start-default': case_time_start_default,
    'riscv-user-counters': case_riscv_user_counters,
    'aarch64-user-counters': case_aarch64_user_counters,
    'user-fork-refused': case_user_fork_refused,
    'system-exact':     case_system_exact,
    'system-vapa':      case_system_vapa,
    'system-modes':     case_system_modes,
    'system-address-spaces': case_system_address_spaces,
    'riscv-user-trace':    case_riscv_user_trace,
    'riscv-smc-rewrite':   case_riscv_smc_rewrite,
    'aarch64-user-simulate': case_aarch64_user_simulate,
    'aarch64-core-configs': case_aarch64_core_configs,
    'riscv-system-simulate': case_riscv_system_simulate,
    'riscv-system-address-spaces': case_riscv_system_address_spaces,
    'riscv-vector-width': case_riscv_vector_width,
    'riscv-power':      case_riscv_power,
    'riscv-user-simulate': case_riscv_user_simulate,
    'riscv-user-translation': case_riscv_user_translation,
    'aarch64-user-translation': case_aarch64_user_translation,
    'record-trace-options': case_record_trace_options,
    'riscv-virtuoso-portable': case_riscv_virtuoso_portable,
    'riscv-user-mt': case_riscv_user_mt,
    'aarch64-user-mt': case_aarch64_user_mt,
    'riscv-user-syscalls': case_riscv_user_syscalls,
    'aarch64-user-syscalls': case_aarch64_user_syscalls,
    'riscv-ime-trace':     case_riscv_ime_trace,
    'riscv-ime-simulate':  case_riscv_ime_simulate,
    'riscv-gemm-verify':   case_riscv_gemm_verify,
    'riscv-gemm-simulate': case_riscv_gemm_simulate,
    'riscv-gemm-sail':     case_riscv_gemm_sail,
    'riscv-llm-agent':     case_riscv_llm_agent,
    'riscv-llm-simulate':  case_riscv_llm_simulate,
    'riscv-ame-verify':    case_riscv_ame_verify,
    'riscv-ame-simulate':  case_riscv_ame_simulate,
    'riscv-ame-couplings': case_riscv_ame_couplings,
    'riscv-llm-couplings': case_riscv_llm_couplings,
    'riscv-size32-verify': case_riscv_size32_verify,
    'riscv-llm-size32':    case_riscv_llm_size32,
    'riscv-vlen-probe':    case_riscv_vlen_probe,
    'riscv-llm-vlen':      case_riscv_llm_vlen,
    'riscv-footprint-lines': case_riscv_footprint_lines,
    'riscv-vector-datapath': case_riscv_vector_datapath,
    'riscv-llm-wide':      case_riscv_llm_wide,
    'riscv-plugin-api':    case_riscv_plugin_api,
    'system-smc-rewrite':  case_system_smc_rewrite,
    'riscv-system-smc-rewrite': case_riscv_system_smc_rewrite,
    'aarch64-smc-rewrite': case_aarch64_smc_rewrite,
    'aarch64-sve-simulate': case_aarch64_sve_simulate,
    'run-sniper-qemu-roi': case_run_sniper_qemu_roi,
    'run-sniper-qemu-system': case_run_sniper_qemu_system,
    'run-sniper-qemu-riscv-units': case_run_sniper_qemu_riscv_units,
    'user-pacsim':         case_user_pacsim,
    'riscv-user-pacsim':   case_riscv_user_pacsim,
    'aarch64-user-pacsim': case_aarch64_user_pacsim,
    'user-pacsim-mt':      case_user_pacsim_mt,
    'riscv-user-pacsim-mt': case_riscv_user_pacsim_mt,
    'aarch64-user-pacsim-mt': case_aarch64_user_pacsim_mt,
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
    ap.add_argument('--keep', action='store_true',
                    help='keep the work directory (guest binaries, traces, outputs; printed at the end) instead of removing it')
    add_failfast(ap)
    args = ap.parse_args()

    p, why = prerequisites()
    if why:
        print('SKIP: %s' % why)
        return 0

    workdir = tempfile.mkdtemp(prefix='sniper-qemu-')
    failures = []
    passed = 0
    log = RunLog('qemu')
    stopped_early = False
    try:
        g = build_guests(workdir)
        for name in (args.case or sorted(CASES)):
            started = time.time()
            try:
                ok, detail = CASES[name](p, g, workdir)
            except Exception as e:
                # a case that raises fails on its own; the others still run
                ok, detail = False, '%s: %s' % (type(e).__name__, str(e).strip().splitlines()[-1] if str(e).strip() else '')
            # None: the case cannot run here (a missing tool), and says why
            verdict = 'SKIP' if ok is None else 'PASS' if ok else 'FAIL'
            took = log.record(name, time.time() - started, verdict)
            print('%-20s %s [%ds]'
                  % (name, 'PASS (%s)' % detail if ok else verdict + ': ' + detail,
                     round(took)))
            if ok:
                passed += 1
            if ok is False:
                failures.append(name)
                if args.failfast:
                    raise FailFast()
    except FailFast:
        stopped_early = True
    finally:
        if stopped_early:
            # The guest binaries live here too, so the failing case can be
            # re-run by hand exactly as it ran.
            print('\nstopped at the first failure; work directory kept at %s'
                  % workdir)
        elif args.keep or failures:      # a failed check keeps the directory whatever --keep says: an intermittent failure leaves its evidence
            print('\nwork directory kept at %s' % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)
        log.finish()

    print()
    total = len(args.case or CASES)
    skipped = total - len(failures) - passed
    if failures:
        print('%d of %d checks FAILED' % (len(failures), total))
        return 1
    print('all %d checks passed%s' % (total - skipped, ' (%d skipped)' % skipped if skipped else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
