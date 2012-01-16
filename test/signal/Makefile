include ../../config/buildconf.makefile

CC=$(GRAPHITE_CC)

all: signal

signal: signal.c Makefile
	$(CC) -o signal signal.c -lm -pthread $(GRAPHITE_LDFLAGS) $(GRAPHITE_CFLAGS)

run: signal
	../../run-sniper -n 2 -c gainestown -g --general/enable_signals=true -- ./signal -p 2

clean:
	rm -f signal signal.o
