# this gives us default build rules and dependency handling
SIM_ROOT ?= $(CURDIR)/../..
PIN_HOME ?= $(SIM_ROOT)/pin_kit
PIN_KIT=$(PIN_HOME)

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# When making clean, do not include this configuration file
# If this is the first fun of a fresh install, this will fail because clean
#  is called, but pin has not been installed yet
ifeq ($(CLEAN),)
include $(PIN_HOME)/source/tools/makefile.gnu.config
endif

LINKER?=${CXX}
CXXFLAGS ?= -std=c++0x -Wall -Wno-unknown-pragmas $(DBG) $(OPT) $(PIN_CXXFLAGS) -I..
LDFLAGS ?= -L.. -lsift -lz

all: sift_recorder


OBJECTS=sift_recorder.o bbv_count.o
sift_recorder: $(OBJECTS) ../libsift.a
	$(CXX) $(PIN_LDFLAGS) -o $@ $(OBJECTS) $(PIN_LIBS) $(LDFLAGS)

include $(SIM_ROOT)/common/Makefile.common

clean:
	rm -f *.o sift_recorder
