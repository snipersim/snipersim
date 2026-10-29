# The guest's timers and clocks in the guest's time under the DynamoRIO front
# end, as test/timers under the SIFT recorder: setitimer, a POSIX timer, a
# timerfd, kept by the front end and fired at their expiry in the simulated
# time, live, and in the one-IPC model's in a trace written to a file -- never
# by the host's kernel (sift/guest_time.h, frontend/dr-frontend/dr_fe_time.cc).
# The guests are testing/apps/timers.c (libc) and timers_raw.c (raw system
# calls); testing/apps/time_probe.c reads clock_gettime, gettimeofday (the
# vDSO's) and rdtsc, which start at the time zero (osemu/time_start).  Needs
# DYNAMORIO=ON; testing/test_apps.py skips the case otherwise.
TARGET=timers
include ../shared/Makefile.shared

APPS=../../testing/apps
CLEAN_EXTRA=timers_raw time_probe

timers: $(APPS)/timers.c timers_raw time_probe
	$(CC) -O1 -pthread $(APPS)/timers.c -pthread -lrt -o timers

timers_raw: $(APPS)/timers_raw.c
	$(CC) -O1 -ffreestanding -nostdlib -static -Wl,-e,_start $(APPS)/timers_raw.c -o timers_raw

time_probe: $(APPS)/time_probe.c
	$(CC) -O1 $(APPS)/time_probe.c -o time_probe

run_$(TARGET):
	../../record-trace --frontend=dr -o $${SNIPER_OUTPUT_DIR:-.}/raw-file -- ./timers_raw | sed 's/^/file: /'
	../../record-trace --frontend=dr -o $${SNIPER_OUTPUT_DIR:-.}/libc-file -- ./timers | sed 's/^/file: /'
	../../record-trace --frontend=dr -o $${SNIPER_OUTPUT_DIR:-.}/probe-file -- ./time_probe | sed 's/^/file: /'
	../../run-sniper --frontend=dr -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/raw-live -- ./timers_raw
	../../run-sniper --frontend=dr -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/libc-live -- ./timers
	../../run-sniper --frontend=dr -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/probe-live -- ./time_probe
