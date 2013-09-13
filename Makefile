SIM_ROOT ?= $(shell readlink -f "$(CURDIR)")

CLEAN=$(findstring clean,$(MAKECMDGOALS))

STANDALONE=$(SIM_ROOT)/lib/sniper
LIB_CARBON=$(SIM_ROOT)/lib/libcarbon_sim.a
LIB_PIN_SIM=$(SIM_ROOT)/pin/../lib/pin_sim.so
LIB_FOLLOW=$(SIM_ROOT)/pin/../lib/follow_execv.so
LIB_SIFT=$(SIM_ROOT)/sift/libsift.a
SIM_TARGETS=$(LIB_CARBON) $(LIB_SIFT) $(LIB_PIN_SIM) $(LIB_FOLLOW) $(STANDALONE)

.PHONY: dependencies compile_simulator configscripts package_deps pin python linux builddir showdebugstatus distclean
# Remake LIB_CARBON on each make invocation, as only its Makefile knows if it needs to be rebuilt
.PHONY: $(LIB_CARBON)

all: dependencies $(SIM_TARGETS) configscripts

dependencies: package_deps pin python mcpat linux builddir showdebugstatus

$(SIM_TARGETS): dependencies

include common/Makefile.common

$(STANDALONE): $(LIB_CARBON) $(LIB_SIFT)
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/standalone

$(LIB_PIN_SIM): $(LIB_CARBON) $(LIB_SIFT)
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/pin $@

$(LIB_FOLLOW):
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/pin $@

$(LIB_CARBON):
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/common

$(LIB_SIFT): $(LIB_CARBON)
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/sift

ifneq ($(NO_PIN_CHECK),1)
PIN_REV_MINIMUM=61206
pin: $(PIN_HOME)/source/tools/Config/makefile.config $(PIN_HOME)/intel64/bin/pinbin package_deps
	@if [ "$$(tools/pinversion.py $(PIN_HOME) | cut -d. -f3)" -lt "$(PIN_REV_MINIMUM)" ]; then echo; echo "Found Pin version $$(tools/pinversion) in $(PIN_HOME)"; echo "but at least revision $(PIN_REV_MINIMUM) is required."; echo; false; fi
$(PIN_HOME)/source/tools/Config/makefile.config:
	@echo
	@echo "Old Pin version found in $(PIN_HOME), Sniper requires Pin version $(PIN_REV_MINIMUM) or newer."
	@echo
	@false
$(PIN_HOME)/intel64/bin/pinbin:
	@echo
	@echo "Cannot find Pin in $(PIN_HOME). Please download and extract Pin version $(PIN_REV_MINIMUM)"
	@echo "from http://www.pintool.org/downloads.html into $(PIN_HOME), or set the PIN_HOME environment variable."
	@echo
	@false
endif

ifneq ($(NO_PYTHON_DOWNLOAD),1)
PYTHON_DEP=python_kit/$(TARGET_ARCH)/lib/python2.7/lib-dynload/_sqlite3.so
python: $(PYTHON_DEP)
$(PYTHON_DEP):
ifeq ($(SHOW_COMPILE),)
	@echo '[DOWNLO] Python $(TARGET_ARCH)'
	@mkdir -p python_kit/$(TARGET_ARCH)
	@wget -O - --no-verbose --quiet "http://snipersim.org/packages/sniper-python27-$(TARGET_ARCH).tgz" | tar xz --strip-components 1 -C python_kit/$(TARGET_ARCH)
else
	mkdir -p python_kit/$(TARGET_ARCH)
	wget -O - --no-verbose --quiet "http://snipersim.org/packages/sniper-python27-$(TARGET_ARCH).tgz" | tar xz --strip-components 1 -C python_kit/$(TARGET_ARCH)
endif
endif

ifneq ($(NO_MCPAT_DOWNLOAD),1)
mcpat: mcpat/mcpatXeonCore.cache
mcpat/mcpatXeonCore.cache:
ifeq ($(SHOW_COMPILE),)
	@echo '[DOWNLO] McPAT'
	@mkdir -p mcpat
	@wget -O - --no-verbose --quiet "http://snipersim.org/packages/mcpat.tgz" | tar xz -C mcpat
else
	mkdir -p mcpat
	wget -O - --no-verbose --quiet "http://snipersim.org/packages/mcpat.tgz" | tar xz -C mcpat
endif
endif

linux: include/linux/perf_event.h
include/linux/perf_event.h:
ifeq ($(SHOW_COMPILE),)
	@echo '[INSTAL] perf_event.h'
	@if [ -e /usr/include/linux/perf_event.h ]; then cp /usr/include/linux/perf_event.h include/linux/perf_event.h; else cp include/linux/perf_event_2.6.32.h include/linux/perf_event.h; fi
else
	if [ -e /usr/include/linux/perf_event.h ]; then cp /usr/include/linux/perf_event.h include/linux/perf_event.h; else cp include/linux/perf_event_2.6.32.h include/linux/perf_event.h; fi
endif

builddir: lib
lib:
	@mkdir -p $(SIM_ROOT)/lib

showdebugstatus:
ifneq ($(DEBUG),)
	@echo Using flags: $(OPT_CFLAGS)
endif

configscripts: dependencies
	@mkdir -p config
	@> config/sniper.py
	@echo '# This file is auto-generated, changes made to it will be lost. Please edit Makefile instead.' >> config/sniper.py
	@echo "target=\"$(TARGET_ARCH)\"" >> config/sniper.py
	@./tools/makerelativepath.py pin_home "$(SIM_ROOT)" "$(PIN_HOME)" >> config/sniper.py
	@if [ $$(which git) ]; then if [ -e "$(SIM_ROOT)/.git" ]; then echo "git_revision=\"$$(git --git-dir='$(SIM_ROOT)/.git' rev-parse HEAD)\"" >> config/sniper.py; fi ; fi
	@./tools/makebuildscripts.py "$(SIM_ROOT)" "$(PIN_HOME)" "$(CC)" "$(CXX)" "$(TARGET_ARCH)"

empty_config:
ifeq ($(SHOW_COMPILE),)
	@echo '[CLEAN ] config'
	@rm -f config/sniper.py config/buildconf.sh config/buildconf.makefile
else
	rm -f config/sniper.py config/buildconf.sh config/buildconf.makefile
endif

clean: empty_config empty_deps
ifeq ($(SHOW_COMPILE),)
	@echo '[CLEAN ] standalone'
	@$(MAKE) $(MAKE_QUIET) -C standalone clean
	@echo '[CLEAN ] pin'
	@$(MAKE) $(MAKE_QUIET) -C pin clean
	@echo '[CLEAN ] common'
	@$(MAKE) $(MAKE_QUIET) -C common clean
	@echo '[CLEAN ] sift'
	@$(MAKE) $(MAKE_QUIET) -C sift clean
	@rm -f .build_os
else
	$(MAKE) $(MAKE_QUIET) -C standalone clean
	$(MAKE) $(MAKE_QUIET) -C pin clean
	$(MAKE) $(MAKE_QUIET) -C common clean
	$(MAKE) $(MAKE_QUIET) -C sift clean
	rm -f .build_os
endif

distclean: clean
ifeq ($(SHOW_COMPILE),)
	@echo '[DISTCL] python_kit'
	@rm -rf python_kit
	@echo '[DISTCL] McPAT'
	@rm -rf mcpat
	@echo '[DISTCL] perf_event.h'
	@rm -f include/linux/perf_event.h
else
	rm -rf python_kit
	rm -rf mcpat
	rm -f include/linux/perf_event.h
endif

regress_quick: regress_unit regress_apps

empty_deps:
ifeq ($(SHOW_COMPILE),)
	@echo '[CLEAN ] deps'
	@find . -name \*.d -exec rm {} \;
else
	find . -name \*.d -exec rm {} \;
endif

package_deps:
	@BOOST_INCLUDE=$(BOOST_INCLUDE) ./tools/checkdependencies.py
