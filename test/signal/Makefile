include ../../config/buildconf.makefile

CC=$(SNIPER_CC)

all: signal

signal: signal.c Makefile
	$(CC) -o signal signal.c -lm -pthread $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

run: signal
	../../run-sniper -n 2 -c gainestown -g --general/enable_signals=true -- ./signal -p 2

clean:
	rm -f signal signal.o
