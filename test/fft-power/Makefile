TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

# The FFT with a McPAT power estimate (run-sniper --power): power.txt,
# power.xml and power.py next to the simulation's output
run_$(TARGET):
	../../run-sniper --roi --power -n 2 -c gainestown -- ./fft -p 2 -t
