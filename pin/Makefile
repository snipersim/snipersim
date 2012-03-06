# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/..

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
	TARGET = ia32
endif
ifeq ($(TARGET_ARCH),x86_64)
	TARGET = ia32e
endif

# When making clean, do not include this configuration file
# If this is the first fun of a fresh install, this will fail because clean
#  is called, but pin has not been installed yet 
ifeq ($(CLEAN),)
PIN_KIT = $(PIN_HOME)
include $(PIN_HOME)/source/tools/makefile.gnu.config
endif

CXXFLAGS += $(PIN_CXXFLAGS)
CFLAGS += $(CXXFLAGS)

# Sources must come before the Makefile.common include to allow for
#  the dependency file generation
SOURCES = $(shell ls $(SIM_ROOT)/pin/*.cc $(SIM_ROOT)/pin/lite/*.cc) $(wildcard $(SIM_ROOT)/pin/libdisasm64/*.c)

OBJECTS = $(patsubst %.c,%.o,$(patsubst %.cc,%.o,$(SOURCES)))

## build rules
TARGET = $(SIM_ROOT)/lib/pin_sim.so

all: $(TARGET)

$(SIM_ROOT)/lib/libcarbon_sim.a:
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/common

$(TARGET): $(PIN_LIBNAMES) $(SIM_ROOT)/lib/libcarbon_sim.a $(SIM_ROOT)/sift/libsift.a
$(TARGET): $(OBJECTS)
ifeq ($(SHOW_COMPILE),)
	@echo '[LD    ]' $(subst $(abspath $(SIM_ROOT))/,,$(abspath $@))
	@$(CXX) $(PIN_LDFLAGS) $(LD_FLAGS) -o $@ $(OBJECTS) $(PIN_LIBS) $(LD_LIBS) $(OPT_CFLAGS) -std=c++0x
else
	$(CXX) $(PIN_LDFLAGS) $(LD_FLAGS) -o $@ $(OBJECTS) $(PIN_LIBS) $(LD_LIBS) $(OPT_CFLAGS) -std=c++0x
endif

# This include must be here
#  - The above targets need to be the default ones.  Makefile.common's would override it
#  - The clean command below must be overwritten by this Makefile to correctly clean 'common'
ifeq ($(CLEAN),)
include $(SIM_ROOT)/common/Makefile.common
endif

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
