SOURCES=$(filter-out siftdump.cc,$(wildcard *.cc))
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))
TARGET=libsift.a
HAVE_LZMA:=$(shell printf '#include <lzma.h>\nint main() { lzma_stream s = LZMA_STREAM_INIT; return (int)s.avail_in; }\n' | $(CXX) -xc++ - -llzma >/dev/null 2>&1 && echo 1 || echo 0)
LZMA_LIBS=$(if $(filter 1,$(HAVE_LZMA)),-llzma,)

include ../common/Makefile.common

ifeq ($(SNIPER_TARGET_ARCH),intel64)
   CXXFLAGS_ARCH=
else
   ifeq ($(SNIPER_TARGET_ARCH),ia32)
      CXXFLAGS_ARCH=-m32
   else
      ifeq ($(SNIPER_TARGET_ARCH),arm32)
         CXXFLAGS_ARCH=
      else
         ifeq ($(SNIPER_TARGET_ARCH),arm64)
            CXXFLAGS_ARCH=
         else
            $(error unknown SNIPER_TARGET_ARCH $(SNIPER_TARGET_ARCH))
         endif
      endif
   endif
endif

all : $(TARGET) siftdump recorder

.PHONY : recorder

include ../common/Makefile.common

CXXFLAGS+=-fPIC $(CXXFLAGS_ARCH)

%.o : %.cc $(wildcard *.h)
	$(_MSG) '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(CXX) -c -o $@ $< $(CXXFLAGS) $(DEPFLAGS)

$(TARGET) : $(OBJECTS)
	$(_MSG) '[AR    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(AR) rcs $@ $^

siftdump : siftdump.o $(TARGET)
	$(_MSG) '[CXX   ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) $(CXX) $(CXXFLAGS_ARCH) -o $@ $^ -L. -lsift -lz -lbz2 $(LZMA_LIBS)

recorder : $(TARGET)
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C recorder -f Makefile

clean :
	$(_CMD) rm -f *.o *.d $(TARGET) siftdump
	$(_MSG) '[CLEAN ] sift/recorder'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C recorder -f Makefile clean

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif
