TARGET=fft
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
#	../../run-sniper -v -n 1 -c gainestown --roi -- ./fft -p 1
	../../run-sniper -v -n 1 -c donuts --roi -- ./fft -p 1

CLEAN_EXTRA=viz
