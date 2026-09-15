TARGET=smc
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	# --smc sets both halves: the simulator knob and the recorder's
	# per-instruction icache.  The config knob alone is not enough --
	# without the recorder's, the trace carries each instruction's
	# original bytes and nothing else, so the rewritten code never
	# reaches the simulator.
	../../run-sniper --smc -n 1 -c gainestown -- ./smc

run-fail: smc
	../../run-sniper -n 1 -c gainestown -- ./smc
