include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: fft
	@echo "\nRun 'make run_random' or 'make run_static' to run the scheduling demo in the simulator\n"

fft.c:
	@ln -s ../fft/fft.c fft.c

fft: fft.c Makefile
	$(CC) -g -o fft fft.c -lm -pthread $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

run: run_random

# '--roi' allows for ROI timing with program hooks instead of full-program timing
# See ${SNIPER_ROOT}/include/sim_api.h for more information
run_random: fft 
	../../run-sniper -n 4 -c gainestown -c small,big,small,big -g --scheduler/type=rand -g --scheduler/quantum=10 -g --scheduler/rand/debug=true  -- ./fft -p 4
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

run_static: fft 
	../../run-sniper -n 4 -c gainestown -c small,big,small,big --roi -g --scheduler/type=static  -g --scheduler/quantum=10  -- ./fft -p 4
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

clean:
	rm -f fft fft.c sim.cfg sim.hetero.cfg sim.stats* *.log *.out *.jpg
