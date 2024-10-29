TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 2 -c gainestown --roi -g perf_model/branch_predictor/type=nn -g perf_model/branch_predictor/batch_length=32 -g perf_model/branch_predictor/learning_rate=0.001 -- ./fft -p 2
