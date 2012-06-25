include ../../config/buildconf.makefile

CC=$(GRAPHITE_CC)

all: fft
	@echo "\nRun 'make run' to run the fft program in the simulator"
	@echo "Run 'make run-iter2' to run the fft program in the simulator, using SimMarker and Python hooks to execute only the second iteration in detailed mode\n"

fft.c:
	@ln -s ../fft/fft.c fft.c

fft: fft.c Makefile
	$(CC) -o fft fft.c -lm -pthread $(GRAPHITE_LDFLAGS) $(GRAPHITE_CFLAGS)

run: fft
	../../run-sniper -n 2 -s markers.py --roi -- ./fft -p 2 -m 6

run-iter2: fft
	../../run-sniper -n 1 -s markers.py -s iter2.py --roi-script --no-cache-warming -- ./fft -p 1 -m 6

clean:
	rm -f fft fft.c sim.cfg sim.stats* *.log *.out
