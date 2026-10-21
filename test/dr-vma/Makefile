# VMA logging (record-trace --vma) by the SDE and the DynamoRIO front ends:
# each records vma.c, and the snapshots that hold its fixed region are printed
# ("vma <front end> snapshots=N first=<icount> region=<first>..<last>"), for
# testing/test_apps.py to check.  Needs DYNAMORIO=ON; the case is skipped
# otherwise.
TARGET=vma
CLEAN_EXTRA=trace-* *.log
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	@rm -f trace-*
	@for fe in sde dr; do \
	  if [ $$fe = dr ]; then args=--frontend=dr; else args=; fi; \
	  ../../record-trace $$args --vma -o trace-$$fe -- ./$(TARGET) > $$fe.log 2>&1 || { echo "vma $$fe record-trace failed"; cat $$fe.log; continue; }; \
	  grep -h 'vma done' $$fe.log; \
	  awk -v fe=$$fe '/^VMA:/ { n++; ic=$$2; if (n == 1) first = ic; next } \
	    $$1 == "200000000-200100000" { if (!lo) lo = n; hi = n } \
	    END { printf "vma %s snapshots=%d first=%s region=%d..%d\n", fe, n, first, lo, hi }' trace-$$fe.vma; \
	done
