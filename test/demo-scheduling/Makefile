include ../../config/buildconf.makefile

CC=$(GRAPHITE_CC)

all: fft
	@echo "\nRun 'make run' to run the fft program in the simulator\n"

fft.c:
	@ln -s ../fft/fft.c fft.c

fft: fft.c Makefile
	$(CC) -g -o fft fft.c -lm -pthread $(GRAPHITE_LDFLAGS) $(GRAPHITE_CFLAGS)

# '--roi' allows for ROI timing with program hooks instead of full-program timing
# See ${GRAPHITE_ROOT}/include/sim_api.h for more information
run_random: fft 
	../../run-sniper -n 4 -c gainestown -c rob -c small,big,small,big -g --perf_model/core/type=rob -g --scheduler/type=rand  -g --scheduler/quantum=10  -- ./fft -p 4
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

run_static: fft 
	../../run-sniper -n 4 -c gainestown -c rob -c small,big,small,big -g --perf_model/core/type=rob --roi -g --scheduler/type=static  -g --scheduler/quantum=10  -- ./fft -p 4
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

clean:
	rm -f fft fft.c sim.cfg sim.stats* *.log *.out *.jpg
