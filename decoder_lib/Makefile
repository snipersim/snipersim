SOURCES=$(wildcard *.cc)
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))
TARGET=libdecoder.a

include ../common/Makefile.common

ifeq ($(SNIPER_TARGET_ARCH),intel64)
	CXXFLAGS_ARCH=
else
	ifeq ($(SNIPER_TARGET_ARCH),ia32)
		CXXFLAGS_ARCH=-m32
	else
		$(error unknown SNIPER_TARGET_ARCH $(SNIPER_TARGET_ARCH))
	endif
endif

ifeq ($(BUILD_RISCV),0)
OBJECTS:=$(filter-out riscv%,$(OBJECTS))
endif

ifeq ($(BUILD_ARM),0)
OBJECTS:=$(filter-out arm%,$(OBJECTS))
endif

all : $(TARGET) 

include ../common/Makefile.common

# rv8 requires at least gcc-5
## The decoder requires gcc-5 as the constexpr from rv8's headers
#CC=gcc-5
#CXX=g++-5

CXXFLAGS+=-std=c++1y -fPIC -I$(XED_HOME)/include/xed -I$(RV8_HOME)/src $(CXXFLAGS_ARCH)
# When compiling Sniper with gcc-4.9, we need to add this compatibility option to allow linking to those binaries
# Add compatibility with the rest of the compiled code
#CXXFLAHS+=-fabi-version=2

%.o : %.cc $(wildcard *.h) Makefile
	$(_MSG) '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@)) 
	$(_CMD) $(CXX) -c -o $@ $< $(CXXFLAGS)

$(TARGET) : $(OBJECTS)
	$(_MSG) '[AR    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(AR) rcs $@ $^

clean :
#	@echo $(XED_HOME)
	$(_CMD) rm -f *.o *.d $(TARGET) 
	$(_MSG) '[CLEAN ] libdecoder'

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif

