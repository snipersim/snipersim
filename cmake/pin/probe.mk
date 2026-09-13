# Ask a Pin or SDE kit for the flags it wants tools built with.
#
# The kit computes these in its own makefile.config, and every Sniper pintool
# previously got them by having its makefile include that file and inherit the
# kit's build rules wholesale -- which is why the recorder and the Pin frontend
# each carried a copy of Pin's makefile scaffolding and each built its own
# private copy of libsift.
#
# Include it once here and print what it produced, so CMake can cache the answer
# and drive the compiles itself.
#
# The base tool flags and the SDE tool flags are kept apart on purpose.  An SDE
# tool is built with -DSDE_INIT -DPINPLAY and the SDE/PinPlay include paths; a
# plain pintool is not, and those defines change which code compiles.  The
# recorder is an SDE tool, the Pin frontend is not.

ifdef SDE_BUILD_KIT
PIN_ROOT := $(SDE_BUILD_KIT)/pinkit
SDE_ROOT := $(PIN_ROOT)/sde-example
PINPLAY_ROOT := $(PIN_ROOT)/pinplay
endif

CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config

# Additions an SDE tool needs; see sift/recorder/makefile.sde.rules.
ifdef SDE_BUILD_KIT
SDE_TOOL_CXXFLAGS := -I$(SDE_ROOT)/include -DSDE_INIT -DPINPLAY -I$(PINPLAY_ROOT)/include
SDE_TOOL_LPATHS := -L$(SDE_ROOT)/lib/$(TARGET) -L$(PINPLAY_ROOT)/$(TARGET)
# sde and pinplay are listed twice on purpose: they are mutually recursive.
SDE_LIBS := -lpinplay -lsde -lpinplay -lsde -lbz2 -lzlib
endif

print-%:
	@echo '$($*)'
