TARGET=fft
CLEAN_EXTRA=fft.c
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET): make_pinball run_pinball

PIN_HOME?=../../pin_kit
PINPLAY_HOME?=$(PIN_HOME)/extras/pinplay
make_pinball: fft
	$(PIN_HOME)/pin -t $(PINPLAY_HOME)/bin/intel64/pinplay-driver.so -log -log:basename fft -log:compressed bzip2  -- ./fft

run_pinball:
	../../run-sniper -n 1 -c gainestown --pinballs=fft.address
