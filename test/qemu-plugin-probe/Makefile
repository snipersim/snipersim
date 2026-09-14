# Standalone: builds against whichever QEMU you point it at.
#
#   make QEMU_SRC=/path/to/qemu-11.1.1   # v7 API (include/plugins/)
#   make QEMU_SRC=/path/to/qemu-9.1.0    # v3 API (include/qemu/)
#
# The same probe.c builds against both; compat.h is the whole difference.

QEMU_SRC ?=
# Needed to build, but not to clean.
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(QEMU_SRC),)
$(error Set QEMU_SRC to a QEMU source tree)
endif
endif

# The plugin header moved in QEMU 11: include/qemu/ -> include/plugins/
PLUGIN_INC := $(firstword $(wildcard $(QEMU_SRC)/include/plugins) \
                          $(wildcard $(QEMU_SRC)/include/qemu))

CFLAGS  += -O2 -Wall -fPIC -I$(PLUGIN_INC) $(shell pkg-config --cflags glib-2.0)

all: probe.so boot.img st mt mtfork

probe.so: probe.c compat.h
	$(CC) $(CFLAGS) -shared -o $@ probe.c

# A boot sector whose instruction count can be computed by hand.
boot.img: boot.S
	$(AS) --32 -o boot.o boot.S
	$(LD) -m elf_i386 -Ttext 0x7c00 --oformat binary -o $@ boot.o

st mt mtfork: %: %.c
	$(CC) -O1 -static -o $@ $< $(if $(filter-out st,$*),-lpthread)

clean:
	rm -f probe.so boot.o boot.img st mt mtfork
.PHONY: all clean
