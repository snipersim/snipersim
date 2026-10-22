# Physical addresses (--pa): simulated under run-sniper, then recorded to a
# trace whose data accesses siftdump prints with their physical address
# ("addr <va>=<pa>"); the summary line is for testing/test_apps.py.
TARGET=pa
CLEAN_EXTRA=trace* record.log
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 1 -c gainestown --pa -- ./$(TARGET)
	@rm -f trace*.sift
	../../record-trace --pa -o trace -- ./$(TARGET) > record.log 2>&1
	@buf=$$(sed -n 's/.*buf=0x\([0-9a-f]*\).*/\1/p' record.log); \
	  $(SNIPER_BUILD_DIR)/lib/siftdump trace.sift 2>/dev/null | grep -o ' [0-9a-f]*=[0-9a-f]*' | \
	  python3 -c 'import sys; lo = int(sys.argv[1], 16); hi = lo + 64 * 4096; n = bad = 0; seen = set()\
	for w in sys.stdin.read().split():\
	    va, pa = (int(x, 16) for x in w.split("="))\
	    if lo <= va < hi: n += 1; bad += pa == 0 or pa == va; seen.add(pa // 4096)\
	print("pa accesses=%d frames=%d unmapped=%d" % (n, len(seen), bad))' $$buf
