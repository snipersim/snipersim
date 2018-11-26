SOURCES=$(filter-out siftdump.cc,$(wildcard *.cc))
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))

ROOT_DIR:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

ifneq ($(DEBUG_SHOW_COMPILE),)
  SHOW_COMPILE=1
  MAKE_QUIET=
  _MSG=@echo >/dev/null
  _CMD=
else
  SHOW_COMPILE=
  MAKE_QUIET=--quiet
  _MSG=@echo
  _CMD=@
endif

all: libsift.a

%.o: %.cc $(wildcard *.h)
	$(_MSG) '[CXX   ]' $(subst $(shell readlink -f $(ROOT_DIR)/../../..)/,,$(shell readlink -f $@))
	$(_CMD) g++ -g -std=c++0x $(COMPILE_PIN_FLAGS) -c -o $@ $<

libsift.a: $(OBJECTS)
	$(_MSG) '[LD    ]' $(subst $(shell readlink -f $(ROOT_DIR)/../../..)/,,$(shell readlink -f $@))
	$(_CMD) ar rcs $@ $(^:%.h=)

# siftdump: siftdump.o libsift.a
# 	g++ -g -std=c++0x -o $@ $< -L$(XED_HOME)/lib -L. -lsift -lxed -lz

clean:
	$(_MSG) '[CLEAN ]' $(subst $(shell readlink -f $(ROOT_DIR)/../../..)/,,$(shell readlink -f $(ROOT_DIR)))
	$(_CMD) rm -f *.o *.a
