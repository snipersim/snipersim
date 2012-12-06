include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: fft
	@echo
	@echo "Run 'make run' to run the fft program in the simulator"
	@echo "Run 'make run-iter2' to run the fft program in the simulator, using SimMarker and Python hooks to execute only the second iteration in detailed mode"
	@echo

fft.c:
	@ln -s ../fft/fft.c fft.c

fft: fft.c Makefile
	$(CC) -o fft fft.c -lm -pthread $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

run: fft
	../../run-sniper -n 2 -s markers:verbose:markers --roi -- ./fft -p 2 -m 6

run-iter2: fft
	../../run-sniper -n 1 -s markers -s roi-iter::2:3 --roi-script --no-cache-warming -- ./fft -p 1 -m 6

clean:
	rm -f fft fft.c sim.cfg sim.stats* *.log *.out
