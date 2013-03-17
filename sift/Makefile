SOURCES=$(filter-out siftdump.cc,$(wildcard *.cc))
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))
TARGET=libsift.a

include ../common/Makefile.common

ifeq ($(TARGET_ARCH),intel64)
   CXXFLAGS_ARCH=
else
   ifeq ($(TARGET_ARCH),ia32)
      CXXFLAGS_ARCH=-m32
   else
      $(error unknown TARGET_ARCH $(TARGET_ARCH))
   endif
endif

all : $(TARGET) siftdump recorder

.PHONY : recorder

include ../common/Makefile.common

XED_HOME=$(PIN_HOME)/extras/xed2-$(TARGET_ARCH)

CXXFLAGS=$(OPT_CFLAGS) -fPIC -std=c++0x -I$(XED_HOME)/include $(CXXFLAGS_ARCH)

%.o : %.cc $(wildcard *.h) Makefile
ifeq ($(SHOW_COMPILE),)
	@echo '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	@$(CXX) -c -o $@ $< $(CXXFLAGS)
else
	$(CXX) -c -o $@ $< $(CXXFLAGS)
endif

$(TARGET) : $(OBJECTS)
ifeq ($(SHOW_COMPILE),)
	@echo '[AR    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	@$(AR) rcs $@ $^
else
	$(AR) rcs $@ $^
endif

siftdump : siftdump.o $(TARGET)
ifeq ($(SHOW_COMPILE),)
	@echo '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	@$(CXX) $(CXXFLAGS_ARCH) -o $@ $^ -L$(XED_HOME)/lib -L. -lsift -lxed -lz
else
	$(CXX) $(CXXFLAGS_ARCH) -o $@ $^ -L$(XED_HOME)/lib -L. -lsift -lxed -lz
endif

recorder : $(TARGET)
	@$(MAKE) $(MAKE_QUIET) -C recorder

clean :
ifeq ($(SHOW_COMPILE),)
	@rm -f *.o *.d $(TARGET) siftdump
	@echo '[CLEAN ] sift/recorder'
	@$(MAKE) $(MAKE_QUIET) -C recorder clean
else
	rm -f *.o *.d $(TARGET) siftdump
	$(MAKE) $(MAKE_QUIET) -C recorder clean
endif

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif
