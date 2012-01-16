include ../../config/buildconf.makefile

CC=$(GRAPHITE_CC)

all: api
	@echo "\nRun 'make run' to run the api program in the simulator\n"

api: api.c Makefile
	$(CC) -o api api.c -lm -pthread $(GRAPHITE_LDFLAGS) $(GRAPHITE_CFLAGS)

# '--roi' allows for ROI timing with program hooks instead of full-program timing
# See ${GRAPHITE_ROOT}/include/sim_api.h for more information
run: api
	../../run-sniper -n 2 -c gainestown --roi -g --api/simple/cores_per_socket=1 -- ./api
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

clean:
	rm -f api sim.cfg sim.stats *.log *.out *.jpg
