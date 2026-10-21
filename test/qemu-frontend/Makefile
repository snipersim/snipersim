# The SPLASH-2 FFT recorded by the QEMU front end (frontend/qemu-plugin) in
# user mode, live, through run-sniper: the same self-checking workload as
# test/fft, with its region of interest, through the third instrumentation
# path.  Needs a build with QEMU=ON (lib/libsniper-qemu.so) and the QEMU
# fetched by deps/sniper-deps; testing/test_apps.py skips the case otherwise.
TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c: ../fft/fft.c
	cp $< $@

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -v -n 1 -c gainestown --roi --frontend=qemu -- ./fft -p 1 -t
