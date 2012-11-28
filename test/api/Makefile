TARGET=api
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 2 -c gainestown -s periodic-stats:10000:1000 --roi -g --api/simple/cores_per_socket=1 -- ./api
