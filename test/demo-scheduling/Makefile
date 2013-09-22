TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET): $(TARGET)
	../../run-sniper -n 4 -c gainestown -c big,big,small,small -g --scheduler/type=big_small -g --scheduler/big_small/quantum=10000 -g --scheduler/big_small/debug=true -ssynctrace -- ./fft -p 4

run_pinned: $(TARGET)
	../../run-sniper -n 2 -c gainestown -c big,small -g --scheduler/type=pinned -g --scheduler/pinned/quantum=10000 -ssynctrace -- ./fft -p 4

run_demo: $(TARGET)
	../../run-sniper -n 4 -c gainestown -c big,big,small,small -g --scheduler/type=pinned -g --scheduler/pinned/quantum=10000 --roi -s demo -- ./fft -p 4 -m 12
