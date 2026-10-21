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
	  awk -v b=$$buf 'BEGIN { lo = strtonum("0x" b); hi = lo + 64 * 4096 } \
	    { split(substr($$1, 1), x, "="); va = strtonum("0x" x[1]); pa = strtonum("0x" x[2]); \
	      if (va < lo || va >= hi) next; n++; if (pa == 0 || pa == va) bad++; seen[int(pa / 4096)] = 1 } \
	    END { k = 0; for (p in seen) k++; printf "pa accesses=%d frames=%d unmapped=%d\n", n, k, bad + 0 }'
