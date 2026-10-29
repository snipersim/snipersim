TARGET=exitalive
include ../shared/Makefile.shared

# Several times at each core count: the shapes differ (with as many cores
# as threads the workers are in the simulation when main returns; with
# fewer, some have never had a core)
RUNS ?= 3

CFLAGS=-O1 -pthread $(SNIPER_CFLAGS)

$(TARGET): ../../testing/apps/$(TARGET).c
	$(CC) $(CFLAGS) ../../testing/apps/$(TARGET).c -pthread $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	@for i in $$(seq 1 $(RUNS)); do \
	  for n in 5 2; do \
	    ../../run-sniper -n $$n -c gainestown -- ./exitalive; \
	    echo "exitalive: run-sniper -n $$n exit status $$?"; \
	  done; \
	done
