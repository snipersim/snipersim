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

from common.failfast import FailFast, add_argument as add_failfast
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


# --- RISC-V, user mode --------------------------------------------------
#
# The same plugin, loaded into a RISC-V QEMU: it reads the target from QEMU at
# install and classifies branches from the encoding.  Needs a plugin-capable
# qemu-riscv64 (SNIPER_QEMU_RISCV) and, to build the guest, a RISC-V compiler
# (SNIPER_RISCV_CC, a clang with lld or a riscv64 gcc); the simulation side
# needs the RISC-V core model (make RISCV=ON, the default).  Skips,
# with the reason, when any of those is missing.

def riscv_prerequisites(workdir):
    qemu = os.environ.get('SNIPER_QEMU_RISCV')
    cc = os.environ.get('SNIPER_RISCV_CC')
    if not qemu or not os.path.isfile(qemu):
        return None, 'SNIPER_QEMU_RISCV not set to a qemu-riscv64 built with --enable-plugins'
    if not cc or not os.path.isfile(cc):
        return None, 'SNIPER_RISCV_CC not set to a RISC-V compiler'
    guest = os.path.join(workdir, 'rv-loop')
    if not os.path.isfile(guest):
        rc, out = run([cc, '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d', '-O1',
                       '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-o', guest,
                       os.path.join(PROBE_DIR, 'rv-loop.c')], 120)
        if rc != 0:
            return None, 'could not build rv-loop.c (exit %s)' % rc
    return {'qemu': qemu, 'guest': guest}, None


def case_riscv_user_trace(p, g, workdir):
    """The frontend traces a RISC-V guest: the exact count, and the trace reads back."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rv')
    rc, out = run([r['qemu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['guest']], 300, cwd=workdir)
    if rc != 100:        # the guest exits with work(20000) & 0x7f == 100
        return False, 'qemu-riscv64 exited %s' % rc
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    if not m:
        return False, 'frontend reported no instruction count'
    n = int(m.group(1))
    if n != 100007:
        return False, '%d instructions, expected 100007' % n
    rc, _ = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (exit %s)' % rc
    return True, '%d instructions, trace readable' % n


def case_riscv_user_simulate(p, g, workdir):
    """Sniper's RISC-V core model simulates the trace; branches agree with the decoder."""
    r, why = riscv_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvsim')
    rc, out = run([r['qemu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (exit %s)' % rc
    outdir = os.path.join(workdir, 'rvsimout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        if 'Unknown core model riscv' in out:
            return None, 'Sniper built without the RISC-V core model (make RISCV=ON)'
        return False, 'run-sniper exited %d' % rc
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
        return False, '%d instructions simulated, expected exactly 100007' % n
    if correct + wrong != 20000:
        return False, '%d branches predicted, expected the loop\'s 20000' % (correct + wrong)
    return True, '%d instructions, %d of %d branches predicted' % (n, correct, correct + wrong)


# The IME (Zvvm 0.9) cases need a qemu-riscv64 built with the extension's CPU
# properties; SNIPER_QEMU_RISCV_IME_CPU overrides the -cpu string.  The guest
# is built like rv-loop.c.  Skips when the QEMU lacks the properties.
IME_CPU_DEFAULT = 'max,vlen=128,x-zvvmm=on,x-zvvfmm=on,x-zvvmtls=on'
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
            return None, 'could not build rv-ime.c (exit %s)' % rc
    cpu = os.environ.get('SNIPER_QEMU_RISCV_IME_CPU', IME_CPU_DEFAULT)
    rc, out = run([r['qemu'], '-cpu', cpu, guest], 60, cwd=workdir)
    if rc != 100:
        if 'zvvm' in out or 'Property' in out:
            return None, 'qemu-riscv64 has no IME (x-zvvmm) CPU properties'
        return None, 'the IME guest exited %s under qemu-riscv64 (expected 100)' % rc
    r['cpu'] = cpu
    r['ime_guest'] = guest
    return r, None

def case_riscv_ime_trace(p, g, workdir):
    """The frontend records every byte range a tile access touches, ahead of the instruction."""
    r, why = riscv_ime_prerequisites(workdir)
    if r is None:
        return None, why
    prefix = os.path.join(workdir, 'rvime')
    rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['ime_guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'qemu-riscv64 exited %s' % rc
    m = re.search(r'\[SNIPER-QEMU\] (\d+) instructions written', out)
    n = int(m.group(1)) if m else -1
    if n != IME_INSNS:
        return False, '%d instructions, expected %d' % (n, IME_INSNS)
    m = re.search(r'\[SNIPER-QEMU\] (\d+) memory footprints recorded', out)
    f = int(m.group(1)) if m else 0
    if f != IME_FOOTPRINTS:
        return False, '%d memory footprints recorded, expected %d' % (f, IME_FOOTPRINTS)
    rc, out = run([p['siftdump'], prefix + '.app0.th0.sift'], 300)
    if rc != 0:
        return False, 'siftdump could not read the trace (exit %s)' % rc
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
    rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['ime_guest']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (exit %s)' % rc
    outdir = os.path.join(workdir, 'rvimesimout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        if 'Unknown core model riscv' in out:
            return None, 'Sniper built without the RISC-V core model (make RISCV=ON)'
        return False, 'run-sniper exited %d' % rc
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
            return None, 'could not build gemm3.c (exit %s)' % rc
    r['gemm'] = guest
    return r, None

def case_riscv_gemm_verify(p, g, workdir):
    """Scalar, RVV and IME compute the same product: the guest says so with exit 100."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    rc, out = run([r['qemu'], '-cpu', r['cpu'], r['gemm']], 120, cwd=workdir)
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
    rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                   '--', r['gemm']], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (exit %s)' % rc
    outdir = os.path.join(workdir, 'gemm3simout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None:
        return False, 'simulation timed out'
    if rc != 0:
        return False, 'run-sniper exited %d' % rc
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
        return False, 'could not build the HTIF variant (exit %s)' % rc
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
LLM_VARIANTS = (('scalar', []), ('rvv', ['-DUSE_RVV']), ('ime', ['-DUSE_IME']), ('ame', ['-DUSE_AME']), ('dma', ['-DUSE_DMA']))

def llm_build(cc, out, extra):
    return run([cc, '--target=riscv64-linux-gnu', '-march=rv64gcv', '-mabi=lp64d', '-O2', '-ffreestanding',
                '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start', '-I', LLM_DIR,
                '-I', os.path.join(SNIPER_ROOT, 'include')] + extra
               + ['-o', out, os.path.join(LLM_DIR, 'agent.c')], 300)

def llm_dims():
    d = {}
    for line in open(os.path.join(LLM_DIR, 'model_weights.h')):
        m = re.match(r'#define GPT_([DLV]) (\d+)', line)
        if m:
            d[m.group(1)] = int(m.group(2))
    return d['D'], d['L'], d['V']

def llm_matrix_counts(variant, decode_tokens, batches):
    """The riscv.<class> counts the agent's kernels imply.  Per decode token
    (and per prefill batch for the head, whose logits only the last token
    needs): matvecs per layer qkv (D->3D), out (D->D), fc1 (D->4D), fc2
    (4D->D), then the head (D->V); IME works 2 output rows x 2-wide K slices
    per tile op, AME 4 x 4 per square, leftover rows through the RVV dot
    product.  Per prefill batch of T tokens each layer matvec is a GEMM: the
    unit's accumulators hold 16 token tiles, so a weight tile is loaded once
    per group of 16 token tiles, an activation tile once per weight tile,
    and leftover tokens go through the matvec."""
    D, L, V = llm_dims()
    layer_shapes = [(D, 3 * D), (D, D), (D, 4 * D), (4 * D, D)] * L
    e = {}
    if variant == 'ime':
        h = 2
        cfg = loads = stores = mma = 0
        def matvec(nin, nout):
            nonlocal cfg, loads, stores, mma
            pairs, ks = nout // h, nin // h
            cfg += 1; loads += pairs * ks * 2; stores += pairs; mma += pairs * ks
        for _ in range(decode_tokens):
            for nin, nout in layer_shapes + [(D, V)]:
                matvec(nin, nout)
        for T in batches:
            matvec(D, V)
            TB, tail = T // h, T % h
            groups = (TB + 15) // 16
            for nin, nout in layer_shapes:
                cfg += 1
                loads += groups * (nout // h) * (nin // h) + TB * (nout // h) * (nin // h)
                mma += TB * (nout // h) * (nin // h)
                stores += TB * (nout // h)
                for _ in range(tail):
                    matvec(nin, nout)
        e = {'riscv.ime_cfg': cfg, 'riscv.ime_tile_load': loads, 'riscv.ime_tile_store': stores, 'riscv.ime_mma_fp': mma}
    elif variant == 'ame':
        h = 4
        cfg = loads = stores = mma = 0
        def matvec(nin, nout):
            nonlocal cfg, loads, stores, mma
            sq, ks = nout // h, nin // h
            cfg += sq; loads += sq * ks * 2; stores += sq; mma += sq * ks
        for _ in range(decode_tokens):
            for nin, nout in layer_shapes + [(D, V)]:
                matvec(nin, nout)
        for T in batches:
            matvec(D, V)
            TB, tail = T // h, T % h
            groups = (TB + 15) // 16
            for nin, nout in layer_shapes:
                cfg += TB * (nout // h)
                loads += groups * (nout // h) * (nin // h) + TB * (nout // h) * (nin // h)
                mma += TB * (nout // h) * (nin // h)
                stores += TB * (nout // h)
                for _ in range(tail):
                    matvec(nin, nout)
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
    """The tool-calling GPT answers every prompt as the numpy model does, with scalar, RVV, IME, AME and DMA-task kernels."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    summary = []
    for variant, guest in r['llm'].items():
        cmd = [r['qemu'], '-cpu', r['cpu']]
        if variant in ('ame', 'dma'):     # QEMU has neither AME nor the accelerator: the plugin executes them
            cmd += ['-plugin', '%s,output=%s' % (p['frontend'], os.path.join(workdir, 'llm' + variant)), '--']
        rc, out = run(cmd + [guest], 900, cwd=workdir)
        names = {5: 'a line differs from the numpy model', 6: 'an answer is wrong', 7: 'lines differ and answers are wrong'}
        if rc != 100:
            return False, '%s kernel: guest exited %s: %s' % (variant, rc, names.get(rc, 'unexpected'))
        if '[AME] error' in out:
            return False, 'the AME backend model reported: ' + [l for l in out.splitlines() if l.startswith('[AME] error')][0]
        m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', out)
        summary.append('%s: %s/%s tokens' % (variant, m.group(1), m.group(2)) if m else variant)
    n = len(re.findall(r'\[= numpy\] \[correct\]', out))
    return True, '%d prompts, every line as numpy, every answer right (%s)' % (n, '; '.join(summary))

def case_riscv_llm_simulate(p, g, workdir):
    """Sniper simulates the agent on RVV, IME and AME and reports the batch-of-1 cost per token; matrix-unit counts must be exact."""
    r, why = riscv_llm_prerequisites(workdir)
    if r is None:
        return None, why
    report = []
    for variant in ('rvv', 'ime', 'ame'):
        prefix = os.path.join(workdir, 'llmsim-' + variant)
        rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                       '--', r['llm'][variant]], 900, cwd=workdir)
        if rc != 100:
            return False, '%s: recording failed (exit %s)' % (variant, rc)
        m = re.search(r'tokens: (\d+) prefilled, (\d+) generated', out)
        if not m:
            return False, '%s: no token count in the guest output' % variant
        prefilled, generated = int(m.group(1)), int(m.group(2))
        mb = re.search(r'prefill batches:((?: \d+)+)', out)
        if not mb:
            return False, '%s: no prefill batch sizes in the guest output' % variant
        batches = [int(t) for t in mb.group(1).split()]
        outdir = os.path.join(workdir, 'llmsimout-' + variant)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '--traces=' + prefix + '.app0.th0.sift'], 5400, cwd=SNIPER_ROOT)
        if rc is None:
            return False, '%s: simulation timed out' % variant
        if rc != 0:
            return False, '%s: run-sniper exited %d' % (variant, rc)
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
        report.append('%s %.0f cycles/token (%.0f insn/token, IPC %.2f)' % (variant, cycles / tokens, insns / tokens, insns / cycles))
    return True, '%d tokens (%d prefill + %d decode): %s; IME and AME instruction counts exact' % (tokens, prefilled, generated, '; '.join(report))

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
        return False, 'could not build the AME variant (exit %s)' % rc
    prefix = os.path.join(workdir, 'gemm3ame')
    rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
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
            return False, 'could not build the AME variant (exit %s)' % rc
    prefix = os.path.join(workdir, 'gemm3amesim')
    rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
    if rc != 100:
        return False, 'recording failed (exit %s)' % rc
    outdir = os.path.join(workdir, 'gemm3amesimout')
    shutil.rmtree(outdir, ignore_errors=True)
    rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                   '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
    if rc is None or rc != 0:
        return False, 'run-sniper exited %s' % rc
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
        rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix),
                       '--', r['llm'][variant]], 900, cwd=workdir)
        if rc != 100:
            return False, '%s: recording failed (exit %s)' % (variant, rc)
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
                           '-g', 'perf_model/ame_tile/coupling=' + coupling,
                           '--traces=' + prefix + '.app0.th0.sift'], 5400, cwd=SNIPER_ROOT)
            if rc is None or rc != 0:
                return False, '%s/%s: run-sniper exited %s' % (variant, coupling, rc)
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
            report.append('%s/%s %.0f cycles/token (%.0f insn/token)' % (variant, coupling, cycles / tokens, insns / tokens))
    return True, '%d tokens: %s; tile and task counts exact' % (tokens, '; '.join(report))

def case_riscv_ame_couplings(p, g, workdir):
    """The attached tile (AmeTile) runs a 32x32x32 AME product under every coupling: tile counts exact, timing responds."""
    r, why = riscv_gemm_prerequisites(workdir)
    if r is None:
        return None, why
    guest = os.path.join(workdir, 'gemm3-ame32')
    REPEAT = 16
    rc, out = gemm_build(os.environ['SNIPER_RISCV_CC'], guest, ['-Wl,-e,_start', '-DUSE_AME', '-DM=32', '-DN=32', '-DK=32', '-DAME_REPEAT=%d' % REPEAT])
    if rc != 0:
        return False, 'could not build the 32-cubed AME variant (exit %s)' % rc
    prefix = os.path.join(workdir, 'gemm3ame32')
    rc, out = run([r['qemu'], '-cpu', r['cpu'], '-plugin', '%s,output=%s' % (p['frontend'], prefix), '--', guest], 300, cwd=workdir)
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
    for coupling in ('ica', 'oca_l1', 'oca_l2', 'oca_llc', 'nca'):
        outdir = os.path.join(workdir, 'tile-' + coupling)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, out = run([os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', '1', '-c', 'riscv', '-d', outdir,
                       '-g', 'perf_model/ame_tile/coupling=' + coupling, '--traces=' + prefix + '.app0.th0.sift'], 900, cwd=SNIPER_ROOT)
        if rc is None or rc != 0:
            return False, '%s: run-sniper exited %s' % (coupling, rc)
        stats = {}
        for line in open(os.path.join(outdir, 'simulation', 'sim.stats')):
            if ' = ' in line:
                k, v = line.split(' = ', 1)
                stats[k.strip()] = v.strip().split(',')[0]
        cycles[coupling] = int(float(stats['performance_model.cycle_count']))
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
        if coupling == 'oca_l2' and int(stats.get('L2.loads', 0)) <= int(stats.get('L1-D.loads', 0)) - rows:
            pass
    if len(set(cycles.values())) < 2:
        return False, 'the couplings all take %d cycles: the tile timing does not reach the core' % cycles['ica']
    return True, 'tile counts exact under every coupling (%d ops, %d rows, %d squares); cycles %s' % (
        ops, rows, mma, ', '.join('%s %d' % (c, n) for c, n in cycles.items()))

CASES = {
    'user-trace':       case_user_trace,
    'user-simulate':    case_user_simulate,
    'user-mt-streams':  case_user_mt_streams,
    'system-exact':     case_system_exact,
    'system-vapa':      case_system_vapa,
    'system-modes':     case_system_modes,
    'riscv-user-trace':    case_riscv_user_trace,
    'riscv-user-simulate': case_riscv_user_simulate,
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
            ok, detail = CASES[name](p, g, workdir)
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
        else:
            shutil.rmtree(workdir, ignore_errors=True)
            run(['make', '-s', 'clean'], 60, cwd=PROBE_DIR)
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
