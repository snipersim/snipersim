SOURCES=$(filter-out siftdump.cc,$(wildcard *.cc))
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))
TARGET=libsift.a

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

all : $(TARGET) siftdump recorder

.PHONY : recorder

include ../common/Makefile.common

XED_HOME=$(PIN_HOME)/extras/xed2-$(SNIPER_TARGET_ARCH)

CXXFLAGS+=-fPIC -I$(XED_HOME)/include $(CXXFLAGS_ARCH)

%.o : %.cc $(wildcard *.h) Makefile
	$(_MSG) '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(CXX) -c -o $@ $< $(CXXFLAGS)

$(TARGET) : $(OBJECTS)
	$(_MSG) '[AR    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(AR) rcs $@ $^

siftdump : siftdump.o $(TARGET)
	$(_MSG) '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(CXX) $(CXXFLAGS_ARCH) -o $@ $^ -L$(XED_HOME)/lib -L. -lsift -lxed -lz

recorder : $(TARGET)
	@$(MAKE) $(MAKE_QUIET) -C recorder

clean :
	$(_CMD) rm -f *.o *.d $(TARGET) siftdump
	$(_MSG) '[CLEAN ] sift/recorder'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C recorder clean

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif
