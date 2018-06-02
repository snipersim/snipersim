SOURCES=$(filter-out siftdump.cc,$(wildcard *.cc))
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))

# ARCH_QUERY = $(shell uname -m)
# ifeq ($(ARCH_QUERY),i686)
# 	TARGET_ARCH = ia32
# 	CXXFLAGS_ARCH=-m32
# else
# 	ifeq ($(ARCH_QUERY),x86_64)
# 		TARGET_ARCH ?= intel64
# 		CXXFLAGS_ARCH=
# 	else
# 		$(error Unknown target arch: $(ARCH_QUERY))
# 	endif
# endif

# XED_HOME=$(wildcard $(PIN_HOME)/extras/xed-$(TARGET_ARCH) $(PIN_HOME)/extras/xed2-$(TARGET_ARCH))

all: libsift.a

%.o: %.cc $(wildcard *.h)
	g++ -g -std=c++0x $(COMPILE_PIN_FLAGS) -c -o $@ $<

libsift.a: $(OBJECTS)
	ar rcs $@ $(^:%.h=)

# siftdump: siftdump.o libsift.a
# 	g++ -g -std=c++0x -o $@ $< -L$(XED_HOME)/lib -L. -lsift -lxed -lz

clean:
	rm -f *.o *.a