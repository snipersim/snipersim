TARGET=smc
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 1 -c gainestown -s periodic-stats:10000:1000 -g general/enable_smc_support=true -- ./smc

run-fail: smc
	../../run-sniper -n 1 -c gainestown -s periodic-stats:10000:1000 -- ./smc
