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
pin: $(PIN_HOME)/intel64/bin/pinbin $(PIN_HOME)/source/tools/Config/makefile.config package_deps
	@if [ "$$(tools/pinversion.py $(PIN_HOME) | cut -d. -f3)" -lt "$(PIN_REV_MINIMUM)" ]; then echo; echo "Found Pin version $$(tools/pinversion.py $(PIN_HOME)) in $(PIN_HOME)"; echo "but at least revision $(PIN_REV_MINIMUM) is required."; echo; false; fi
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
PYTHON_DEP=python_kit/$(SNIPER_TARGET_ARCH)/lib/python2.7/lib-dynload/_sqlite3.so
python: $(PYTHON_DEP)
$(PYTHON_DEP):
	$(_MSG) '[DOWNLO] Python $(SNIPER_TARGET_ARCH)'
	$(_CMD) mkdir -p python_kit/$(SNIPER_TARGET_ARCH)
	$(_CMD) wget -O - --no-verbose --quiet "http://snipersim.org/packages/sniper-python27-$(SNIPER_TARGET_ARCH).tgz" | tar xz --strip-components 1 -C python_kit/$(SNIPER_TARGET_ARCH)
endif

ifneq ($(NO_MCPAT_DOWNLOAD),1)
mcpat: mcpat/mcpat-1.0
mcpat/mcpat-1.0:
	$(_MSG) '[DOWNLO] McPAT'
	$(_CMD) mkdir -p mcpat
	$(_CMD) wget -O - --no-verbose --quiet "http://snipersim.org/packages/mcpat-1.0.tgz" | tar xz -C mcpat
endif

linux: include/linux/perf_event.h
include/linux/perf_event.h:
	$(_MSG) '[INSTAL] perf_event.h'
	$(_CMD) if [ -e /usr/include/linux/perf_event.h ]; then cp /usr/include/linux/perf_event.h include/linux/perf_event.h; else cp include/linux/perf_event_2.6.32.h include/linux/perf_event.h; fi

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
	@echo "target=\"$(SNIPER_TARGET_ARCH)\"" >> config/sniper.py
	@./tools/makerelativepath.py pin_home "$(SIM_ROOT)" "$(PIN_HOME)" >> config/sniper.py
	@if [ $$(which git) ]; then if [ -e "$(SIM_ROOT)/.git" ]; then echo "git_revision=\"$$(git --git-dir='$(SIM_ROOT)/.git' rev-parse HEAD)\"" >> config/sniper.py; fi ; fi
	@./tools/makebuildscripts.py "$(SIM_ROOT)" "$(PIN_HOME)" "$(CC)" "$(CXX)" "$(SNIPER_TARGET_ARCH)"

empty_config:
	$(_MSG) '[CLEAN ] config'
	$(_CMD) rm -f config/sniper.py config/buildconf.sh config/buildconf.makefile

clean: empty_config empty_deps
	$(_MSG) '[CLEAN ] standalone'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C standalone clean
	$(_MSG) '[CLEAN ] pin'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C pin clean
	$(_MSG) '[CLEAN ] common'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C common clean
	$(_MSG) '[CLEAN ] sift'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C sift clean
	$(_MSG) '[CLEAN ] tools'
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C tools clean
	$(_CMD) rm -f .build_os

distclean: clean
	$(_MSG) '[DISTCL] python_kit'
	$(_CMD) rm -rf python_kit
	$(_MSG) '[DISTCL] McPAT'
	$(_CMD) rm -rf mcpat
	$(_MSG) '[DISTCL] perf_event.h'
	$(_CMD) rm -f include/linux/perf_event.h

regress_quick: regress_unit regress_apps

empty_deps:
	$(_MSG) '[CLEAN ] deps'
	$(_CMD) find . -name \*.d -exec rm {} \;

package_deps:
	@BOOST_INCLUDE=$(BOOST_INCLUDE) ./tools/checkdependencies.py
