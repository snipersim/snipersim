include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: fft
	@echo "\nRun 'make run' to run the fft program in the simulator\n"

fft.c:
	@ln -s ../fft/fft.c fft.c

fft: fft.c Makefile
	$(CC) -o fft fft.c -lm -pthread $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

# '--roi' allows for ROI timing with program hooks instead of full-program timing
# See ${SNIPER_ROOT}/include/sim_api.h for more information
run: fft
	../../run-sniper -n 4 -c gainestown --roi -c hetero.cfg -g --perf_model/core/interval_timer/window_size=128,96,64,32 -- ./fft -p 4
	@echo "\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run\n"

clean:
	rm -f fft fft.c sim.cfg sim.stats* *.log *.out *.jpg
