TARGET=fft
CLEAN_EXTRA=fft.c fft-pinball
include ../shared/Makefile.shared

fft.c:
	@ln -s ../fft/fft.c fft.c

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o -lm $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET): make_pinball run_pinball

# PinPlay comes with SDE rather than as a separate driver.
#
# This used to build extras/pinplay/examples out of a pin_kit and run
# pinplay-driver.so.  Neither exists: the tree ships an SDE kit, whose
# pinkit has no extras/pinplay, so the recording step failed and the replay
# had nothing to read.  SDE has the logger and the replayer built in, as the
# -log and -replay knobs on sde64 itself -- which is what record-trace has
# always used for the replay half.
#
# PIN_HOME comes from buildconf.makefile and points at sde_kit/pinkit.
SDE_HOME ?= $(PIN_HOME)/..

# Makefile is a prerequisite: the pinball is a recording of a particular command
# line, so changing that command has to invalidate it.  Without this, adding the
# workload's -t self-check left the old pinball in place and the replay went on
# faithfully replaying a run made without it.
#
# So is the kit stamp.  A pinball is not portable across SDE versions, and
# updating the kit under one recorded by the previous version left the replay
# producing no output at all -- which reads as a workload failure rather than as
# a stale recording.  The kit's own mtime is no use here (it is the tarball's,
# always older than the pinball), so the stamp holds the resolved kit path,
# which is content-addressed and therefore changes exactly when the kit does.
fft-pinball/.sde-kit: FORCE
	@mkdir -p fft-pinball
	@echo '$(realpath $(SDE_HOME))' | cmp -s - $@ 2>/dev/null \
	  || echo '$(realpath $(SDE_HOME))' > $@

.PHONY: FORCE
FORCE:

fft-pinball/pinball.address: fft Makefile fft-pinball/.sde-kit
	@mkdir -p fft-pinball
	$(SDE_HOME)/sde64 -log -log:basename fft-pinball/pinball -log:compressed bzip2 -- ./fft -p 1 -m 6 -t

make_pinball: fft-pinball/pinball.address

# --pinplay-addr-trans is required, not optional: a pinball recorded on one run
# is replayed into a different address layout, and without it the replay stops
# with "Cannot read page into address ... please use -replay:addr_trans knob"
# and simulates zero instructions.
run_pinball: fft-pinball/pinball.address
	../../run-sniper -n 1 -c gainestown --pinballs=fft-pinball/pinball.address --pinplay-addr-trans
