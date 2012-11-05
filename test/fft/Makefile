include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: fft
	@echo -e "\nRun 'make run' to run the fft program in the simulator\n"

fft: fft.c Makefile
	$(CC) -o fft fft.c -lm -pthread $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

# '--roi' allows for ROI timing with program hooks instead of full-program timing
# See ${SNIPER_ROOT}/include/sim_api.h for more information
run: fft
	../../run-sniper -n 2 -c gainestown --roi -- ./fft -p 2
	@echo -e "\n\nOptional: Run '../../tools/cpistack.py' in this directory to generate cpi-stack output for this run"
	@echo -e "Optional: Run '../../tools/mcpat.py' in this directory to generate power output for this run\n"

clean:
	rm -f fft sim.cfg sim.stats* *.log *.out *.jpg
