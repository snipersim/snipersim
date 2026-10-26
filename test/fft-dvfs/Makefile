TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 2 -c gainestown --roi -g --dvfs/simple/cores_per_socket=1 -sdvfs:1000:1:1000 -- ./fft -p 2 -t   # core 1 to 1 GHz after 1 us of the ROI (scripts/dvfs.py)
