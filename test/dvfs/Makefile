include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: dvfs
	@echo "\nRun 'make run' to run the dvfs program in the simulator\n"

dvfs: dvfs.c Makefile
	$(CC) -o dvfs dvfs.c -lm -pthread $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

# '--roi' allows for ROI timing with program hooks instead of full-program timing
# See ${SNIPER_ROOT}/include/sim_api.h for more information
run: dvfs
	../../run-sniper -n 2 -c gainestown --roi -g --dvfs/simple/cores_per_socket=1 -- ./dvfs
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

clean:
	rm -f dvfs sim.cfg sim.stats* *.log *.out *.jpg
