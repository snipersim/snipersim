all: run

# Recorded with ASLR off: the trace is then the same bytes every time, and
# the checks that read it (testing/test_virtuoso.py's MimicOS cases) can
# expect exact counts.  Recorded with ASLR on, two recordings differ, and
# so did the page-fault count a check expected.  The guest's addresses also
# follow its environment and its path, so the harness records this trace
# itself under a controlled environment (test_virtuoso.sniper_in_sniper_trace)
# and keeps the environment's digest beside it in fft.sift.env; a trace
# recorded here, under make's environment, is replaced by the harness.
SETARCH := $(shell command -v setarch >/dev/null 2>&1 && echo "setarch -R")
fft.sift:
	make -C ../fft fft
	$(SETARCH) ../../record-trace --roi -o fft -- ../fft/fft

# The outer simulator's region of interest is the inner simulator's whole
# run (TraceManager::start to stop), MimicOS start-up included: tens of
# billions of instructions, hours in cache-only mode.  The outer therefore
# fast-forwards through it, which still records the region's statistics and
# checks what this test is for, that one simulator runs inside another.
run: fft.sift
	../../run-sniper --wrap-sim="../../run-sniper -n4 -cgainestown -ccacheonly -ggeneral/inst_mode_roi=fast_forward --no-cache-warming -gscheduler/type=roaming --roi -sprogresstrace -- " -n2 --traces=fft,fft -sstop-by-icount:10000 -gcore/hook_periodic_ins/ins_global=10000 -d $${SNIPER_OUTPUT_DIR:-.}/sim-inside
