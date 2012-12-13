# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/../..
PIN_HOME ?= $(SIM_ROOT)/pin_kit
PIN_KIT=$(PIN_HOME)

all: sift_recorder

include $(SIM_ROOT)/common/Makefile.common

# Translate Sniper's target names into Pin target name
ifeq ($(TARGET_ARCH),intel64)
   TARGET=ia32e
else
   ifeq ($(TARGET_ARCH),ia32)
      TARGET=ia32
   else
      $(error unknown TARGET_ARCH $(TARGET_ARCH))
   endif
endif

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# When making clean, do not include this configuration file
# If this is the first fun of a fresh install, this will fail because clean
#  is called, but pin has not been installed yet
ifeq ($(CLEAN),)
include $(PIN_HOME)/source/tools/makefile.gnu.config
endif

LINKER?=${CXX}
CXXFLAGS += -std=c++0x -Wall -Wno-unknown-pragmas $(DBG) $(OPT_CFLAGS) $(PIN_CXXFLAGS) -I..
LDFLAGS += -L.. -lsift -lz

PINPLAY_HOME=$(PIN_HOME)/extras/pinplay
ifneq ($(wildcard $(PINPLAY_HOME)/include/pinplay.H),)
CXXFLAGS += -DPINPLAY_SUPPORTED -D_FILE_OFFSET_BITS=64 -I$(PINPLAY_HOME)/include
LDFLAGS += -L$(PINPLAY_HOME)/lib/$(TARGET_ARCH) -lsift -lpinplay -lbz2 -lz
endif

SOURCES=$(wildcard *.cc)
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))
sift_recorder: $(OBJECTS) ../libsift.a
ifeq ($(SHOW_COMPILE),)
	@echo '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	@$(CXX) $(PIN_LDFLAGS) -o $@ $(OBJECTS) $(LDFLAGS) $(PIN_LIBS)
else
	$(CXX) $(PIN_LDFLAGS) -o $@ $(OBJECTS) $(LDFLAGS) $(PIN_LIBS)
endif

include $(SIM_ROOT)/common/Makefile.common

clean:
ifeq ($(SHOW_COMPILE),)
	@rm -f *.o sift_recorder
else
	rm -f *.o sift_recorder
endif

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif
