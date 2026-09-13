# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/..

LD_LIBS += -lcarbon_sim -lpthread

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# Use these files for auto targets
.SUFFIXES:  .o .c .h .cc

# Add other CXX Flags
CXXFLAGS += -c \
            -fPIC -Wall -Wno-unknown-pragmas $(OPT_CFLAGS) #-Werror

# Use the pin flags for building
include $(SIM_ROOT)/Makefile.config

# Sources must come before the Makefile.common include to allow for
#  the dependency file generation
SOURCES = $(shell ls $(SIM_ROOT)/standalone/*.cc)

OBJECTS = $(patsubst %.c,%.o,$(patsubst %.cc,%.o,$(SOURCES)))

## build rules
TARGET = $(SIM_ROOT)/lib/sniper

all: $(TARGET)

# The libraries below are built by the top-level Makefile, which orders them
# before recursing here ($(STANDALONE): $(LIB_CARBON) $(LIB_SIFT) $(LIB_DECODER)).
#
# This used to recurse into common/ to build libcarbon_sim.a, and pin/Makefile
# declared the very same rule, so under -j two make processes could write one
# 200 MiB archive at the same time.  sift/libsift.a and decoder_lib/libdecoder.a
# meanwhile were listed as prerequisites with no rule at all, which worked only
# because the parent happened to build them first -- `make -n` from the top level
# failed outright on it.
#
# Make the top level the single owner of all three, and fail with something
# actionable rather than a bare "No rule to make target" if one is missing.
$(SIM_ROOT)/lib/libcarbon_sim.a $(SIM_ROOT)/sift/libsift.a $(SIM_ROOT)/decoder_lib/libdecoder.a:
	@echo >&2
	@echo "        $@ is missing." >&2
	@echo "        Build from the top level:  make -C $(SIM_ROOT)" >&2
	@echo >&2
	@false

$(TARGET): $(SIM_ROOT)/lib/libcarbon_sim.a $(SIM_ROOT)/sift/libsift.a $(SIM_ROOT)/decoder_lib/libdecoder.a
$(TARGET): $(OBJECTS)
	$(_MSG) '[LD    ]' $(subst $(SIM_ROOT_ABS)/,,$(abspath $@))
	$(_CMD) $(CXX) $(LD_FLAGS) -o $@ $(OBJECTS) $(LD_LIBS) $(OPT_CFLAGS) -std=c++0x

# This include must be here
#  - The above targets need to be the default ones.  Makefile.common's would override it
#  - The clean command below must be overwritten by this Makefile to correctly clean 'common'
ifeq ($(CLEAN),)
include $(SIM_ROOT)/common/Makefile.common
endif

ifeq ($(SNIPER_TARGET_ARCH),intel64)
   CXXFLAGS_ARCH=
else
   ifeq ($(SNIPER_TARGET_ARCH),ia32)
      CXXFLAGS_ARCH=-m32
   else
      $(error unknown SNIPER_TARGET_ARCH $(SNIPER_TARGET_ARCH))
   endif
endif

# These libraries are used by libcarbon, so add them to the end
LD_LIBS += -lxed
LD_FLAGS += -L$(XED_HOME)/lib -no-pie

ifneq ($(CLEAN),clean)
-include $(patsubst %.cpp,%.d,$(patsubst %.c,%.d,$(patsubst %.cc,%.d,$(SOURCES))))
endif

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
