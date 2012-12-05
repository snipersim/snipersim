TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 4 -c gainestown --roi -c hetero.cfg -g --perf_model/core/interval_timer/window_size=128,96,64,32 -- ./fft -p 4
