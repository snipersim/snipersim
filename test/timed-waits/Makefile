# The guest's timeouts in the guest's time under the SIFT recorder: every
# timed wait ends at its deadline in the simulated time, live, and in the
# one-IPC model's in a trace written to a file -- never in the host's
# (sift/guest_time.h).  The guests are testing/apps/timed_raw.c (raw system
# calls) and timed_waits.c (libc); testing/test_qemu.py runs them under QEMU.
TARGET=timed_waits
include ../shared/Makefile.shared

APPS=../../testing/apps
CLEAN_EXTRA=timed_raw

timed_waits: $(APPS)/timed_waits.c timed_raw
	$(CC) -O1 -pthread $(APPS)/timed_waits.c -pthread -o timed_waits

timed_raw: $(APPS)/timed_raw.c
	$(CC) -O1 -ffreestanding -nostdlib -static -Wl,-e,_start $(APPS)/timed_raw.c -o timed_raw

# To a file the recorder takes one thread only: the waits another thread ends
# run live
FILE_WAITS=nanosleep,clock_nanosleep-abs,poll,ppoll,select,epoll_wait,sigtimedwait,cond_timedwait-realtime,sem_timedwait,poll-2s

run_$(TARGET):
	../../record-trace -o $${SNIPER_OUTPUT_DIR:-.}/raw-file -- ./timed_raw | sed 's/^/file: /'
	TIMED_WAITS_ONLY=$(FILE_WAITS) ../../record-trace -o $${SNIPER_OUTPUT_DIR:-.}/libc-file -- ./timed_waits | sed 's/^/file: /'
	../../run-sniper -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/raw-live -- ./timed_raw
	../../run-sniper -n 2 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/libc-live -- ./timed_waits
