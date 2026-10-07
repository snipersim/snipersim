# The SPLASH-2 FFT recorded by the DynamoRIO front end (frontend/dr-frontend)
# instead of the SDE recorder: the same self-checking workload as test/fft,
# through the other instrumentation path.  Needs a build with DYNAMORIO=ON
# (lib/libdr-frontend.so) and the DynamoRIO fetched by deps/sniper-deps;
# testing/test_apps.py skips the case when either is missing.
TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c: ../fft/fft.c
	cp $< $@

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -v -n 1 -c gainestown --roi --frontend=dr -- ./fft -p 1 -t
