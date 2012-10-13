include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: smc
	@echo "\nRun 'make run' to run the smc program in the simulator with SMC support"
	@echo "Run 'make run-fail' to run the smc program in the simulator without SMC support\n"

smc: smc.c Makefile
	$(CC) -o smc smc.c $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

run: smc
	../../run-sniper -n 1 -c gainestown -g general/enable_smc_support=true -- ./smc
run-fail: smc
	../../run-sniper -n 1 -c gainestown -- ./smc

clean:
	rm -f smc sim.cfg sim.stats* *.log *.out
