TARGET=smc
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	# The default: code rewritten in place is noticed when Pin
	# re-translates it, and its page is sent to the simulator again.
	../../run-sniper -n 1 -c gainestown -- ./smc

# The strict mode: every instruction is checked against the bytes last
# sent (slower; for code the default cannot see change).
run-strict: smc
	../../run-sniper --smc -n 1 -c gainestown -- ./smc

# Without tracking, the trace carries the original bytes of the rewritten
# code and the program goes wrong in simulation.
run-fail: smc
	../../run-sniper --no-smc -n 1 -c gainestown -- ./smc
