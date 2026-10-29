TARGET=sleepexit
include ../shared/Makefile.shared

CFLAGS=-O1 -std=gnu99 -pthread $(SNIPER_CFLAGS)

# The source is shared with testing/test_qemu.py (user-sleep-exit).
$(TARGET).o: ../../testing/apps/$(TARGET).c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -pthread $(SNIPER_LDFLAGS) -o $(TARGET)

# The main thread sleeps while the worker runs and exits, then the worker
# sleeps too.  Both used to hang (ThreadManager::onThreadExit).
run_$(TARGET):
	../../run-sniper -n 2 -c gainestown -- ./sleepexit 20000
	../../run-sniper -n 2 -c gainestown -- ./sleepexit 20000 2000
