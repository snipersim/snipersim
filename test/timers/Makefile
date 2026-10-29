# The guest's timers in the guest's time under the SIFT recorder: setitimer,
# a POSIX timer, a timerfd, kept by the recorder and fired at their expiry in
# the simulated time, live, and in the one-IPC model's in a trace written to
# a file -- never by the host's kernel (sift/guest_time.h).  The guests are
# testing/apps/timers.c (libc) and timers_raw.c (raw system calls);
# testing/test_qemu.py runs them under QEMU.
TARGET=timers
include ../shared/Makefile.shared

APPS=../../testing/apps
CLEAN_EXTRA=timers_raw

timers: $(APPS)/timers.c timers_raw
	$(CC) -O1 -pthread $(APPS)/timers.c -pthread -lrt -o timers

timers_raw: $(APPS)/timers_raw.c
	$(CC) -O1 -ffreestanding -nostdlib -static -Wl,-e,_start $(APPS)/timers_raw.c -o timers_raw

run_$(TARGET):
	../../record-trace -o $${SNIPER_OUTPUT_DIR:-.}/raw-file -- ./timers_raw | sed 's/^/file: /'
	../../record-trace -o $${SNIPER_OUTPUT_DIR:-.}/libc-file -- ./timers | sed 's/^/file: /'
	../../run-sniper -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/raw-live -- ./timers_raw
	../../run-sniper -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/libc-live -- ./timers
