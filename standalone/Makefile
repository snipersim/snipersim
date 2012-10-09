# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/..

LD_LIBS += -lcarbon_sim -lpthread

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# Use these files for auto targets
.SUFFIXES:  .o .c .h .cc

# Add other CXX Flags
CXXFLAGS += -c \
            -Wall -Wno-unknown-pragmas $(OPT_CFLAGS) #-Werror

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
	@echo '[LD    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
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

ifeq ($(TARGET_ARCH),intel64)
   CXXFLAGS_ARCH=
else
   ifeq ($(TARGET_ARCH),ia32)
      CXXFLAGS_ARCH=-m32
   else
      $(error unknown TARGET_ARCH $(TARGET_ARCH))
   endif
endif

# These libraries are used by libcarbon, so add them to the end
LD_LIBS += -lxed
LD_FLAGS += -L$(PIN_HOME)/extras/xed2-$(TARGET_ARCH)/lib

ifneq ($(CLEAN),clean)
-include $(patsubst %.cpp,%.d,$(patsubst %.c,%.d,$(patsubst %.cc,%.d,$(SOURCES))))
endif

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
