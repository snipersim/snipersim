TARGET=spinloop
include ../shared/Makefile.shared

CFLAGS=-O2 -std=c99 -pthread $(SNIPER_CFLAGS)

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -pthread $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 2 -c gainestown --roi -gcore/spin_loop_detection=true -- ./spinloop
	../../tools/dumpstats.py | grep spin
	@# sim.out moved into simulation/ when run-sniper started sorting its
	@# output into subdirectories; accept either layout.
	head -n 4 simulation/sim.out 2>/dev/null || head -n 4 sim.out
