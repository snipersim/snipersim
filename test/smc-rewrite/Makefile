# Code rewritten in place (see smc_rewrite.c), recorded by the SDE front end.
# Two halves: the program simulated end to end (the simulator must decode the
# rewritten instruction, not reuse the old decode), and the program recorded
# to a trace whose bytes at the rewritten address are printed, one line per
# execution ("traced <pc> <bytes>"), so the check can see that each run was
# traced with its own code.  FRONTEND_ARGS selects the front end (the
# dr-smc-rewrite case passes --frontend=dr).
TARGET=smc_rewrite
FRONTEND_ARGS ?=
CLEAN_EXTRA=trace* record.log
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 1 -c gainestown $(FRONTEND_ARGS) -- ./$(TARGET)
	@rm -f trace*.sift
	../../record-trace $(FRONTEND_ARGS) -o trace -- ./$(TARGET) > record.log 2>&1
	@pc=$$(sed -n 's/.*B rewritten pc=0x\([0-9a-f]*\).*/\1/p' record.log); \
	  $(SNIPER_BUILD_DIR)/lib/siftdump trace.sift 2>/dev/null | grep -i "^0*$$pc " | sed "s/^0*\([0-9a-f]*\) */traced \1 /"
