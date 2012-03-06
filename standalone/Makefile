# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/..

LD_LIBS += -lcarbon_sim

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# Use these files for auto targets
.SUFFIXES:  .o .c .h .cc

# Add other CXX Flags
CXXFLAGS += -c \
            -Wall -Wno-unknown-pragmas $(OPT_CFLAGS) #-Werror

LD_FLAGS += -L$(PIN_HOME)/extras/xed2-intel64/lib

# Use the pin flags for building
include $(SIM_ROOT)/Makefile.config

# Sources must come before the Makefile.common include to allow for
#  the dependency file generation
SOURCES = $(shell ls $(SIM_ROOT)/standalone/*.cc)

OBJECTS = $(patsubst %.c,%.o,$(patsubst %.cc,%.o,$(SOURCES)))

## build rules
TARGET = $(SIM_ROOT)/lib/sniper

all: $(TARGET)

$(SIM_ROOT)/lib/libcarbon_sim.a:
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/common

$(TARGET): $(SIM_ROOT)/lib/libcarbon_sim.a $(SIM_ROOT)/sift/libsift.a
$(TARGET): $(OBJECTS)
ifeq ($(SHOW_COMPILE),)
	@echo '[LD    ]' $(subst $(abspath $(SIM_ROOT))/,,$(abspath $@))
	@$(CXX) $(LD_FLAGS) -o $@ $(OBJECTS) $(LD_LIBS) $(OPT_CFLAGS) -std=c++0x
else
	$(CXX) $(LD_FLAGS) -o $@ $(OBJECTS) $(LD_LIBS) $(OPT_CFLAGS) -std=c++0x
endif

# This include must be here
#  - The above targets need to be the default ones.  Makefile.common's would override it
#  - The clean command below must be overwritten by this Makefile to correctly clean 'common'
ifeq ($(CLEAN),)
include $(SIM_ROOT)/common/Makefile.common
endif

# These libraries are used by libcarbon, so add them to the end
LD_LIBS += -lxed

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
