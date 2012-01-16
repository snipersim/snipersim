SIM_ROOT ?= $(CURDIR)
GRAPHITE_ROOT ?= SIM_ROOT

CLEAN=$(findstring clean,$(MAKECMDGOALS))

LIB_CARBON=$(SIM_ROOT)/lib/libcarbon_sim.a
LIB_PIN_SIM=$(SIM_ROOT)/pin/../lib/pin_sim.so
LIB_USER=$(SIM_ROOT)/lib/libgraphite_user.a

all: package_deps pin python linux output_files builddir $(LIB_CARBON) $(LIB_PIN_SIM) $(LIB_USER) configscripts

# Include common here
# - This allows us to override clean below
include common/Makefile.common

.PHONY: $(LIB_PIN_SIM) $(LIB_CARBON)

$(LIB_PIN_SIM):
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/pin $@

$(LIB_CARBON):
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/common

$(LIB_USER):
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/user

ifneq ($(NO_PIN_CHECK),1)
pin: $(PIN_HOME)/intel64/bin/pinbin
$(PIN_HOME)/intel64/bin/pinbin:
	@echo "\nCannot find Pin in $(PIN_HOME). Please download and extract Pin into $(PIN_HOME),"
	@echo "or set the PIN_HOME environment variable.\n"
	@false
endif

ifneq ($(NO_PYTHON_DOWNLOAD),1)
python: python_kit/include/python2.7/Python.h
python_kit/include/python2.7/Python.h:
	wget -O - --no-verbose "http://snipersim.org/packages/sniper-python27.tgz" | tar xz
endif

linux: include/linux/perf_event.h
include/linux/perf_event.h:
	if [ -e /usr/include/linux/perf_event.h ]; then cp /usr/include/linux/perf_event.h include/linux/perf_event.h; else cp include/linux/perf_event_2.6.32.h include/linux/perf_event.h; fi

builddir: lib
lib:
	@mkdir -p $(SIM_ROOT)/lib

showdebugstatus:
ifneq ($(DEBUG),)
	@echo Using flags: $(OPT_CFLAGS)
endif

configscripts:
	@mkdir -p config
	@> config/graphite.py
	@echo '# This file is auto-generated, changes made to it will be lost. Please edit Makefile instead.' >> config/graphite.py
	@./tools/makerelativepath.py pin_home "$(SIM_ROOT)" "$(PIN_HOME)" >> config/graphite.py
	@./tools/makerelativepath.py boost_lib "$(SIM_ROOT)" "$(BOOST_LIB)" >> config/graphite.py
	@if [ -e .git ]; then echo "git_revision=\"$(git rev-parse HEAD)\"" >> config/graphite.py; fi
	@./tools/makebuildscripts.py "$(SIM_ROOT)" "$(BOOST_LIB)" "$(BOOST_SUFFIX)" "$(PIN_HOME)" "$(CC)" "$(CXX)"

empty_config:
	rm -f config/graphite.py config/buildconf.sh config/buildconf.makefile

clean: empty_logs empty_config empty_deps
	$(MAKE) $(MAKE_QUIET) -C pin clean
	$(MAKE) $(MAKE_QUIET) -C common clean
	$(MAKE) $(MAKE_QUIET) -C user clean
	@rm -f .build_os

regress_quick: output_files regress_unit regress_apps

output_files:
	mkdir output_files

empty_logs :
	rm output_files/* ; true

empty_deps:
	find . -name \*.d -exec rm {} \;

package_deps:
	@./tools/checkdependencies.py
