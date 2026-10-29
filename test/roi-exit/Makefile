TARGET=roi_exit
include ../shared/Makefile.shared

# Each shape several times: what failed here depended on timing
RUNS ?= 3

CFLAGS=-O1 -pthread $(SNIPER_CFLAGS)

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -pthread $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	@for i in $$(seq 1 $(RUNS)); do \
	  for shape in join exit; do \
	    ../../run-sniper -n 5 -c gainestown --roi -- ./roi_exit $$shape; \
	    echo "roi-exit: run-sniper exit status $$?"; \
	  done; \
	done
