TARGET=spinloop
include ../shared/Makefile.shared

CFLAGS=-O2 -std=c99 -pthread $(SNIPER_CFLAGS)

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -pthread $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 2 -c gainestown --roi -gcore/spin_loop_detection=true -- ./spinloop
	../../tools/dumpstats.py | grep spin
	head -n 4 sim.out
