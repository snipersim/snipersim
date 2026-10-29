# After the region of interest a live run stays live: the clock goes on,
# threads started after the ROI are the simulator's, joins and timed waits
# are emulated in simulated time.  The guests are testing/apps/roi_time.c and
# roi_threads.c (run under QEMU by testing/test_qemu.py too).
TARGET=roi_threads
include ../shared/Makefile.shared

APPS=../../testing/apps
CFLAGS=-O1 -pthread $(SNIPER_CFLAGS)
CLEAN_EXTRA=roi_time

roi_threads: $(APPS)/roi_threads.c roi_time
	$(CC) $(CFLAGS) $(APPS)/roi_threads.c -pthread $(SNIPER_LDFLAGS) -o roi_threads

roi_time: $(APPS)/roi_time.c
	$(CC) $(CFLAGS) $(APPS)/roi_time.c $(SNIPER_LDFLAGS) -o roi_time

run_$(TARGET):
	../../run-sniper -n 2 -c gainestown --roi -d $${SNIPER_OUTPUT_DIR:-.}/time -- ./roi_time
	../../run-sniper -n 4 -c gainestown --roi -d $${SNIPER_OUTPUT_DIR:-.}/threads -- ./roi_threads
