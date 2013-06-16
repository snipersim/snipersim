# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/..

# Note: Pin's makefile.config adds stuff to the `all' target which we don't want,
#       while the target architecture is specified in TARGET so just some different names here
default: $(TARGET_LIB) $(FOLLOW_LIB)

LD_LIBS += -lcarbon_sim

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# Use these files for auto targets
.SUFFIXES:  .o .c .h .cc

# Add other CXX Flags
CXXFLAGS += -c \
            -Wall -Wno-unknown-pragmas $(OPT_CFLAGS) #-Werror

# Use the pin flags for building
include $(SIM_ROOT)/Makefile.config

ifeq ($(TARGET_ARCH),ia32)
   override TARGET:=ia32
endif
ifeq ($(TARGET_ARCH),intel64)
   override TARGET:=intel64
endif

# When making clean, do not include this configuration file
# If this is the first fun of a fresh install, this will fail because clean
#  is called, but pin has not been installed yet
ifeq ($(CLEAN),)
override PIN_ROOT := $(PIN_HOME)
CONFIG_ROOT := $(PIN_HOME)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config
endif

CXXFLAGS += $(TOOL_CXXFLAGS)
CFLAGS += $(CXXFLAGS)

PINPLAY_HOME=$(PIN_HOME)/extras/pinplay
ifneq ($(wildcard $(PINPLAY_HOME)/include/pinplay.H),)
CXXFLAGS += -DPINPLAY_SUPPORTED -D_FILE_OFFSET_BITS=64 -I$(PINPLAY_HOME)/include
LD_LIBS += -lpinplay -lbz2 -lz
LD_FLAGS += -L$(PINPLAY_HOME)/lib/$(TARGET_ARCH)
endif

# Sources must come before the Makefile.common include to allow for
#  the dependency file generation
SOURCES = $(wildcard $(SIM_ROOT)/pin/*.cc $(SIM_ROOT)/pin/lite/*.cc)

OBJECTS = $(patsubst %.c,%.o,$(patsubst %.cc,%.o,$(SOURCES)))

## build rules
TARGET_LIB = $(SIM_ROOT)/lib/pin_sim.so
FOLLOW_LIB = $(SIM_ROOT)/lib/follow_execv.so

$(SIM_ROOT)/lib/libcarbon_sim.a:
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/common

$(TARGET_LIB): $(SIM_ROOT)/lib/libcarbon_sim.a $(SIM_ROOT)/sift/libsift.a
$(TARGET_LIB): $(OBJECTS)
ifeq ($(SHOW_COMPILE),)
	@echo '[LD    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	@$(CXX) $(TOOL_LDFLAGS) $(LD_FLAGS) -o $@ $(OBJECTS) $(LD_LIBS) $(TOOL_LPATHS) $(TOOL_LIBS) $(OPT_CFLAGS) -std=c++0x
else
	$(CXX) $(TOOL_LDFLAGS) $(LD_FLAGS) -o $@ $(OBJECTS) $(LD_LIBS) $(TOOL_LPATHS) $(TOOL_LIBS) $(OPT_CFLAGS) -std=c++0x
endif

$(FOLLOW_LIB): follow_execv/follow_execv.o
ifeq ($(SHOW_COMPILE),)
	@echo '[LD    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	@$(CXX) $(TOOL_LDFLAGS) $(LD_FLAGS) -o $@ follow_execv/follow_execv.o $(TOOL_LPATHS) $(TOOL_LIBS) $(OPT_CFLAGS) -std=c++0x
else
	$(CXX) $(TOOL_LDFLAGS) $(LD_FLAGS) -o $@ follow_execv/follow_execv.o $(TOOL_LPATHS) $(TOOL_LIBS) $(OPT_CFLAGS) -std=c++0x
endif

# This include must be here
#  - The above targets need to be the default ones.  Makefile.common's would override it
#  - The clean command below must be overwritten by this Makefile to correctly clean 'common'
ifeq ($(CLEAN),)
include $(SIM_ROOT)/common/Makefile.common
endif

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET_LIB) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
