#!/usr/bin/env python3
"""Expected instruction-class and memory-footprint counts of test/riscv-gemm/gemm3.c,
derived from the specification's semantics, checked against a Sniper run.

    expected_counts.py <sim.stats> [--vlen 128] [--sew 32]

The RVV path does one K-long dot product per C element: two unit-stride
vector loads (vle32.v: vl elements of SEW/8 bytes each, contiguous), one
vmacc.vv per vector-length chunk of K, one vmv.v.i, one vmv.s.x, one vredsum.vs, one vmv.x.s, a vsetvli before, per chunk and after.  The IME
path works on tiles whose shape the UDB defines through the vtype geometry
(vmtl.v: `n = M * linesize` elements with `linesize = lambda * LMUL`,
`M = (VLEN/SEW) / lambda`; each element SEW/8 bytes; element i lives at
`base + (i / linesize) * LD + (i % linesize)` elements): per output tile
one C tile load, per 2-wide K slice one A tile load, one B tile load and
one vmmacc.vv, then one C tile store; plus one vsetvli and one vsetlambda.
Byte footprints follow: a tile moves M*linesize*SEW/8 bytes; a vector load
vl*SEW/8.  Sniper's riscv.* counters (per thread) and the recorded span
bytes must match these exactly; the cache-level load and store counts
must be at least the vector and tile accesses (scalar traffic adds to
them)."""
import argparse, sys

def expected(vlen, sew, m, n, k, lam=2, lmul=1, ame=False):
    epr = vlen // sew
    linesize = lam * lmul
    rows = epr // lam
    tile_elems = rows * linesize
    assert tile_elems == 4 and rows == 2 and linesize == 2, 'gemm3.c assumes 2x2 tiles'
    tiles_c = (m // rows) * (n // linesize)
    kslices = k // linesize
    e = {}
    vlmax = 2 * epr                                  # the dot product runs e32,m2
    iters = (k + vlmax - 1) // vlmax                 # vector-length chunks of the K loop
    e['riscv.v_cfg'] = (2 + iters) * m * n + 1       # vsetvli before, per chunk and after each dot product; one for the IME path
    e['riscv.v_load'] = 2 * m * n * iters
    e['riscv.v_int_mul'] = m * n * iters             # vmacc.vv per chunk
    e['riscv.v_int_alu'] = 4 * m * n                 # vmv.v.i, vmv.s.x, vredsum.vs, vmv.x.s
    e['riscv.ime_cfg'] = 1                           # vsetlambda
    e['riscv.ime_tile_load'] = tiles_c * (1 + 2 * kslices)
    e['riscv.ime_tile_store'] = tiles_c
    e['riscv.ime_mma_int'] = tiles_c * kslices
    v_bytes = 2 * m * n * k * (sew // 8)             # the vle32.v chunks together move K elements per operand
    t_bytes = (e['riscv.ime_tile_load'] + e['riscv.ime_tile_store']) * tile_elems * (sew // 8)
    e['riscv.span_bytes'] = v_bytes + t_bytes
    e['riscv.spans'] = None                          # lines touched: layout-dependent, reported, not asserted
    if ame:
        # AME path (gemm3.c -DUSE_AME): 4x4 Int32 squares, output squares (m/4)*(n/4),
        # per output square one asettyp, per 4-wide K slice two strided loads and one
        # mmulbtacc.2d, then one mmov.m.a and one strided store; three msettyp, an
        # acquire and a release.  A strided square is 4 rows of 16 bytes.
        sq_out = (m // 4) * (n // 4); ksl = k // 4
        e['riscv.ame_own'] = 2
        e['riscv.ame_cfg'] = 3 + sq_out
        e['riscv.ame_load'] = sq_out * 2 * ksl
        e['riscv.ame_store'] = sq_out
        e['riscv.ame_mma'] = sq_out * ksl
        e['riscv.ame_move'] = sq_out
        e['riscv.span_bytes'] += (e['riscv.ame_load'] + e['riscv.ame_store']) * 64
    e['_min_dcache_loads'] = e['riscv.v_load'] + e['riscv.ime_tile_load']
    e['_min_dcache_stores'] = e['riscv.ime_tile_store']
    return e

def read_stats(path):
    stats = {}
    for line in open(path):
        if ' = ' in line:
            kk, v = line.split(' = ', 1)
            stats[kk.strip()] = sum(int(x) for x in v.strip().split(',') if x.strip().lstrip('-').isdigit())
    return stats

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stats'); ap.add_argument('--vlen', type=int, default=128); ap.add_argument('--sew', type=int, default=32)
    ap.add_argument('--m', type=int, default=8); ap.add_argument('--n', type=int, default=8); ap.add_argument('--k', type=int, default=8)
    ap.add_argument('--ame', action='store_true', help='the build also ran the AME path (-DUSE_AME)')
    a = ap.parse_args()
    e = expected(a.vlen, a.sew, a.m, a.n, a.k, ame=a.ame)
    s = read_stats(a.stats)
    ok = True
    for key, want in e.items():
        if key.startswith('_') or want is None:
            continue
        got = s.get(key)
        flag = 'ok  ' if got == want else 'FAIL'
        ok &= got == want
        print('%s %-24s expected %6d  got %6s' % (flag, key, want, got))
    print('     %-24s (layout-dependent)   got %6s' % ('riscv.spans', s.get('riscv.spans')))
    for key, cnt in (('L1-D.loads', e['_min_dcache_loads']), ('L1-D.stores', e['_min_dcache_stores'])):
        got = s.get(key, 0)
        flag = 'ok  ' if got >= cnt else 'FAIL'
        ok &= got >= cnt
        print('%s %-24s at least %6d  got %6s' % (flag, key, cnt, got))
    print('compute: %d vmmacc.vv over %d tiles, %d vmacc.vv; memory: %d bytes of vector/tile footprint'
          % (e['riscv.ime_mma_int'], e['riscv.ime_tile_store'], e['riscv.v_int_mul'], e['riscv.span_bytes']))
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
