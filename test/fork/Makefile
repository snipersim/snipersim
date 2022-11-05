TARGET=fork
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
#	../../run-sniper -n 2 -c gainestown --sift -- ./fork
	../../run-sniper -n 2 -c writethrough --sift -- ./fork
