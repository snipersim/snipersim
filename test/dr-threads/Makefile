# A multi-threaded program recorded by the DynamoRIO front end: the
# scripting suite's threaded workload (main plus four workers, each summing
# a different harmonic tail, then joined), on four cores with syscall
# emulation, so the front end must announce every thread glibc creates
# (clone3 since glibc 2.34) and pair each with its own pthread_join word.
# The printed total is only right when every join waited for its own worker.
# Needs a build with DYNAMORIO=ON; testing/test_apps.py skips the case
# otherwise.
TARGET=threaded
CLEAN_EXTRA=threaded.c
include ../shared/Makefile.shared

threaded.c: ../../testing/apps/threaded.c
	cp $< $@

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -pthread $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -v -n 4 -c gainestown --frontend=dr -- ./threaded
