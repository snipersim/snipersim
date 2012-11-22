TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 4 -c gainestown -c small,big,small,big -g --scheduler/type=rand -g --scheduler/quantum=10 -g --scheduler/rand/debug=true  -- ./fft -p 4

run_pinned:
	../../run-sniper -n 2 -c gainestown -c small,big -g --scheduler/type=pinned -g --scheduler/pinned/quantum=1000000 -- ./fft -p 4

run_static: fft 
	../../run-sniper -n 4 -c gainestown -c small,big,small,big --roi -g --scheduler/type=static  -g --scheduler/quantum=10  -- ./fft -p 4
