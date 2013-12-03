all: run

fft.sift:
	make -C ../fft fft
	../../record-trace --roi -o fft -- ../fft/fft

run: fft.sift
	../../run-sniper --wrap-sim="../../run-sniper -n4 -cgainestown -ccacheonly --no-cache-warming -gscheduler/type=roaming --roi -sprogresstrace -- " -n2 --traces=fft,fft -sstop-by-icount:10000 -gcore/hook_periodic_ins/ins_global=10000 -d sim-inside
