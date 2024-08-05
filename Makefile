SIM_ROOT ?= $(shell readlink -f "$(CURDIR)")

CLEAN=$(findstring clean,$(MAKECMDGOALS))

STANDALONE=$(SIM_ROOT)/lib/sniper
PIN_FRONTEND=$(SIM_ROOT)/frontend/pin-frontend/obj-intel64/pin_frontend
DYNAMORIO_FRONTEND=$(SIM_ROOT)/frontend/dr-frontend/build/libdr-frontend.so
LIB_CARBON=$(SIM_ROOT)/lib/libcarbon_sim.a
LIB_PIN_SIM=$(SIM_ROOT)/pin/../lib/pin_sim.so
LIB_FOLLOW=$(SIM_ROOT)/pin/../lib/follow_execv.so
LIB_SIFT=$(SIM_ROOT)/sift/libsift.a
LIB_DECODER=$(SIM_ROOT)/decoder_lib/libdecoder.a
SIM_TARGETS=$(LIB_DECODER) $(LIB_CARBON) $(LIB_SIFT) $(LIB_PIN_SIM) $(LIB_FOLLOW) $(STANDALONE) $(PIN_FRONTEND) $(DYNAMORIO_FRONTEND)

PYTHON2=python2

.PHONY: all message dependencies compile_simulator configscripts package_deps pin python linux builddir showdebugstatus distclean mbuild xed_install xed
# Remake LIB_CARBON on each make invocation, as only its Makefile knows if it needs to be rebuilt
.PHONY: $(LIB_CARBON)

all: message dependencies $(SIM_TARGETS) configscripts

#NOTE PINPLAY: This is not updated to the latest version: 1) SDE already contains pinplay, 2) the pinplay-driver (inside the pinplay_toolkit github) cannot easily be build for PIN. 3) The latest pinplay version has a bug preventing to read pinballs.
#KNOWN ISSUE WITH SDE: It cannot run properly inside virtualbox

# Check for errors. Only one value should be set
TARGET_COUNT:=0
# For Pin
ifeq (,$(USE_PIN))
# Do nothing
else ifeq ($(USE_PIN),1)
TARGET_COUNT:=$(shell echo $$(($(TARGET_COUNT)+1)))
else # USE_PIN != "1"
$(error If using, set USE_PIN to "1", not "$(USE_PIN)")
endif
# For Pinplay
ifeq (,$(USE_PINPLAY))
# Do nothing
else ifeq ($(USE_PINPLAY),1)
TARGET_COUNT:=$(shell echo $$(($(TARGET_COUNT)+1)))
else # USE_PINPLAY != "1"
$(error If using, set USE_PINPLAY to "1", not "$(USE_PINPLAY)")
endif
# For SDE
ifeq (,$(USE_SDE))
# Do nothing
else ifeq ($(USE_SDE),1)
TARGET_COUNT:=$(shell echo $$(($(TARGET_COUNT)+1)))
else # USE_SDE != "1"
$(error If using, set USE_SDE to "1", not "$(USE_SDE)")
endif
# Set the default if no values are set
ifeq ($(TARGET_COUNT),0)
USE_PIN=1
# USE_SDE=1
# export USE_SDE #Makefile.config needs that variable
else ifeq ($(TARGET_COUNT),1)
# Input is valid. Use user-supplied default
else
# Error, cannot be >= 2
$(error One or more tools requested for build. Only one supported USE_PIN=$(USE_PIN) USE_PINPLAY=$(USE_PINPLAY) USE_SDE=$(USE_SDE))
endif

include common/Makefile.common

dependencies: package_deps sde_kit $(PIN_ROOT) pin xed python mcpat linux builddir showdebugstatus

BUILD_CAPSTONE ?=
ifeq ($(BUILD_ARM),1)
BUILD_CAPSTONE=1
else ifeq ($(BUILD_DYNAMORIO),1)
BUILD_CAPSTONE=1
endif

ifeq ($(BUILD_CAPSTONE),1)
dependencies: capstone
.PHONY: capstone
endif

ifeq ($(BUILD_DYNAMORIO),1)
dependencies: dynamorio
.PHONY: dynamorio
endif

$(SIM_TARGETS): dependencies

message:
	@echo -n Building for x86 \($(SNIPER_TARGET_ARCH)\)
ifneq (,$(USE_PIN))
	@echo -n " with Pin"
else ifneq (,$(USE_PINPLAY))
	@echo -n " with Pinplay"
else
	@echo -n " with SDE"
endif
ifeq ($(BUILD_DYNAMORIO),1)
	@echo -n " and DynamoRIO"
endif
ifeq ($(BUILD_RISCV),1)
	@echo -n " and RISCV"
endif
ifeq ($(BUILD_ARM),1)
	@echo -n " and arm64"
endif
	@echo ""

$(STANDALONE): $(LIB_CARBON) $(LIB_SIFT) $(LIB_DECODER)
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/standalone

$(PIN_FRONTEND):
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/frontend/pin-frontend

ifeq ($(BUILD_DYNAMORIO),1)
$(DYNAMORIO_FRONTEND): $(LIB_SIFT)
	@if [ ! -e "$(SIM_ROOT)/frontend/dr-frontend/build/Makefile" ]; then mkdir -p $(SIM_ROOT)/frontend/dr-frontend/build && cd $(SIM_ROOT)/frontend/dr-frontend/build && cmake -DDEBUG=ON -DDynamoRIO_DIR=$(DYNAMORIO_INSTALL)/build/cmake .. ; fi
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/frontend/dr-frontend/build
else
$(DYNAMORIO_FRONTEND):
	$(_CMD) true
endif

# Disable original frontend

#$(LIB_PIN_SIM): $(LIB_CARBON) $(LIB_SIFT) $(LIB_DECODER)
#	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/pin $@

#$(LIB_FOLLOW):
#	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/pin $@

$(LIB_CARBON): 
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/common

$(LIB_SIFT): $(LIB_CARBON)
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/sift

$(LIB_DECODER): $(LIB_CARBON)
	@$(MAKE) $(MAKE_QUIET) -C $(SIM_ROOT)/decoder_lib 

# DYNAMORIO_GITID=246ddb28e7848b2d09d2b9909f99a6da9b2ce35e
DYNAMORIO_INSTALL=$(SIM_ROOT)/dynamorio
DYNAMORIO_INSTALL_DEP=$(DYNAMORIO_INSTALL)/CMakeLists.txt
$(DYNAMORIO_INSTALL_DEP):
	$(_MSG) '[DOWNLO] dynamorio'
	$(_CMD) git submodule update --init --recursive --quiet dynamorio
	$(_CMD) touch $(DYNAMORIO_INSTALL)/.autodownloaded

DYNAMORIO_BUILD_DEP=$(DYNAMORIO_INSTALL)/build/bin64/drrun
dynamorio: $(DYNAMORIO_BUILD_DEP)
$(DYNAMORIO_BUILD_DEP): $(DYNAMORIO_INSTALL_DEP)
	$(_MSG) '[INSTAL] dynamorio'
	$(_CMD) if [ ! -e "$(SIM_ROOT)/dynamorio/build/Makefile" ]; then cd dynamorio && mkdir build && cd build && cmake -DDEBUG=ON .. ; fi
	$(_CMD) $(MAKE) $(MAKE_QUIET) -C dynamorio/build

# CAPSTONE_GITID=f9c6a90489be7b3637ff1c7298e45efafe7cf1b9
CAPSTONE_INSTALL=$(SIM_ROOT)/capstone
CAPSTONE_INSTALL_DEP=$(CAPSTONE_INSTALL)/arch/AArch64/ARMMappingInsnOp.inc
$(CAPSTONE_INSTALL_DEP):
	$(_MSG) '[DOWNLO] capstone'
	$(_CMD) git submodule update --init --quiet capstone
	$(_CMD) touch $(CAPSTONE_INSTALL)/.autodownloaded

CAPSTONE_BUILD_DEP=$(CAPSTONE_INSTALL)/libcapstone.so.4
capstone: $(CAPSTONE_BUILD_DEP)
$(CAPSTONE_BUILD_DEP): $(CAPSTONE_INSTALL_DEP)
	$(_MSG) '[INSTAL] capstone'
	$(_CMD) cd $(CAPSTONE_INSTALL) ; ./make.sh


MBUILD_INSTALL=$(SIM_ROOT)/mbuild
MBUILD_INSTALL_DEP=$(MBUILD_INSTALL)/mbuild/arar.py
mbuild: $(MBUILD_INSTALL_DEP)
$(MBUILD_INSTALL_DEP):
	$(_MSG) '[DOWNLO] mbuild'
	$(_CMD) git submodule update --init --quiet mbuild
	$(_CMD) 

XED_INSTALL_DEP=$(XED_INSTALL)/src/common/xed-init.c
xed_install: $(XED_INSTALL_DEP)
$(XED_INSTALL_DEP):
	$(_MSG) '[DOWNLO] xed'
	$(_CMD) git submodule update --init --quiet xed

XED_DEP=$(XED_HOME)/include/xed/xed-iclass-enum.h
xed: mbuild xed_install $(XED_DEP)
$(XED_DEP): $(XED_INSTALL_DEP)
	$(_MSG) '[INSTAL] xed'
	$(_CMD) cd $(XED_INSTALL) ; ./mfile.py --silent --extra-flags=-fPIC --shared --install-dir $(XED_HOME) install

ifneq (,$(USE_PIN))
PIN_DOWNLOAD=https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-3.31-98861-g71afcc22f-gcc-linux.tar.gz
PIN_DEP=$(PIN_HOME)/intel64/lib/libpindwarf.so
$(PIN_ROOT): $(PIN_DEP)
$(PIN_DEP):
	$(_MSG) '[DOWNLO] Pin'
	$(_CMD) mkdir -p $(PIN_HOME)
	$(_CMD) wget -O $(shell basename $(PIN_DOWNLOAD)) $(WGET_OPTIONS) --no-verbose --quiet $(PIN_DOWNLOAD)
	$(_CMD) tar -x -f $(shell basename $(PIN_DOWNLOAD)) --auto-compress --strip-components 1 -C $(PIN_HOME)
	$(_CMD) rm $(shell basename $(PIN_DOWNLOAD))
	$(_CMD) touch $(PIN_HOME)/.autodownloaded
sde_kit: $(PIN_ROOT)
else ifneq (,$(USE_PINPLAY))
PIN_DOWNLOAD=https://snipersim.org/packages/pinplay-dcfg-3.11-pin-3.11-97998-g7ecce2dac-gcc-linux.tar.bz2
PIN_DEP=$(PIN_HOME)/intel64/lib-ext/libpin3dwarf.so
$(PIN_ROOT): $(PIN_DEP)
$(PIN_DEP):
	$(_MSG) '[DOWNLO] Pinplay 3.11-97998'
	$(_CMD) mkdir -p $(PIN_HOME)
	$(_CMD) wget -O $(shell basename $(PIN_DOWNLOAD)) $(WGET_OPTIONS) --no-verbose --quiet $(PIN_DOWNLOAD)
	$(_CMD) tar -x -f $(shell basename $(PIN_DOWNLOAD)) --auto-compress --strip-components 1 -C $(PIN_HOME)
	$(_CMD) rm $(shell basename $(PIN_DOWNLOAD))
	$(_CMD) touch $(PIN_HOME)/.autodownloaded
sde_kit: $(PIN_ROOT)
else
# SDE_DOWNLOAD=https://snipersim.org/packages/sde-external-9.7.0-2022-05-09-lin.tar.xz
SDE_DOWNLOAD=https://downloadmirror.intel.com/823664/sde-external-9.38.0-2024-04-18-lin.tar.xz
PIN_DEP=$(SDE_HOME)/intel64/pin_lib/libpindwarf.so
sde_kit: $(PIN_DEP)
$(PIN_DEP):
	$(_MSG) '[DOWNLO] SDE'
	$(_CMD) mkdir -p $(SDE_HOME)
	$(_CMD) wget -O $(shell basename $(SDE_DOWNLOAD)) $(WGET_OPTIONS) --no-verbose --quiet $(SDE_DOWNLOAD)
	$(_CMD) tar -x -f $(shell basename $(SDE_DOWNLOAD)) --auto-compress --strip-components 1 -C $(SDE_HOME)
	$(_CMD) rm $(shell basename $(SDE_DOWNLOAD))
	$(_CMD) rm -r $(SDE_HOME)/pinkit/source/tools/InstLib #It looks like it is missing source files, so it fails to compile but not necessary for sniper
	$(_CMD) touch $(SDE_HOME)/.autodownloaded
	$(_MSG) '[DOWNLO] pinplay-tools'
	$(_CMD) git submodule update --init --quiet $(SDE_HOME)/pinplay-tools
$(PIN_ROOT): sde_kit
endif

ifneq ($(NO_PIN_CHECK),1)
PIN_REV_MINIMUM=71313
pin: $(PIN_DEP)
	@if [ "$$(tools/pinversion.py $(PIN_HOME) | cut -d. -f3)" -lt "$(PIN_REV_MINIMUM)" ]; then echo; echo "Found Pin version $$(tools/pinversion.py $(PIN_HOME)) in $(PIN_HOME)"; echo "but at least revision $(PIN_REV_MINIMUM) is required."; echo; false; fi
endif

PYTHON_DEP=python_kit/$(SNIPER_TARGET_ARCH)/pyvenv.cfg
python: $(PYTHON_DEP)
$(PYTHON_DEP):
	$(_MSG) '[INSTAL] Python virtualenv python_kit/$(SNIPER_TARGET_ARCH)'
	$(_CMD) python3 -m venv python_kit/$(SNIPER_TARGET_ARCH)

ifneq ($(NO_MCPAT_DOWNLOAD),1)
mcpat/main.cc:
	$(_MSG) '[DOWNLO] McPAT'
	$(_CMD) git submodule update --init --quiet mcpat

mcpat: mcpat/mcpat-1.0
mcpat/mcpat-1.0: mcpat/main.cc
	$(_MSG) '[INSTAL] mcpat'
	$(_CMD) SUFFIX=-1.0 make -C mcpat
	$(_CMD) SUFFIX=-1.0.cache CACHE=1 make -C mcpat
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
	@./tools/makerelativepath.py sde_home "$(SIM_ROOT)" "$(SDE_HOME)" >> config/sniper.py
	@./tools/makerelativepath.py pin_home "$(SIM_ROOT)" "$(PIN_HOME)" >> config/sniper.py
	@./tools/makerelativepath.py xed_home "$(SIM_ROOT)" "$(XED_HOME)" >> config/sniper.py
	@./tools/makerelativepath.py dynamorio_home "$(SIM_ROOT)" "$(DYNAMORIO_INSTALL)/build" >> config/sniper.py
	@if [ $$(which git) ]; then if [ -e "$(SIM_ROOT)/.git" ]; then echo "git_revision=\"$$(git --git-dir='$(SIM_ROOT)/.git' rev-parse HEAD)\"" >> config/sniper.py; fi ; fi
	@./tools/makebuildscripts.py "$(SIM_ROOT)" "$(SDE_HOME)" "$(PIN_HOME)" "$(DYNAMORIO_INSTALL)/build" "$(CC)" "$(CXX)" "$(SNIPER_TARGET_ARCH)"

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
	$(_MSG) '[CLEAN ] frontend/pin-frontend'
	$(_CMD) if [ -d "$(PIN_HOME)" ]; then $(MAKE) $(MAKE_QUIET) -C frontend/pin-frontend clean ; fi
	$(_MSG) '[CLEAN ] frontend/dr-frontend'
	$(_CMD) if [ -d "$(SIM_ROOT)/frontend/dr-frontend/build" ]; then rm -rf $(SIM_ROOT)/frontend/dr-frontend/build ; fi
	$(_MSG) '[CLEAN ] McPat'
	$(_CMD) if [ -e mcpat/makefile ]; then $(MAKE) $(MAKE_QUIET) SUFFIX=-1.0 -C mcpat clean; fi
	$(_CMD) if [ -e mcpat/makefile ]; then $(MAKE) $(MAKE_QUIET) SUFFIX=-1.0.cache -C mcpat clean; fi
	$(_CMD) rm -f .build_os

distclean: clean
	$(_MSG) '[DISTCL] Pin kit'
	$(_CMD) if [ -e "$(PIN_HOME)/.autodownloaded" ]; then rm -rf $(PIN_HOME); fi
	$(_CMD) if [ -e "pin_kit/.autodownloaded" ]; then rm -rf pin_kit; fi
	$(_MSG) '[DISTCL] SDE kit'
	$(_CMD) if [ -e "$(SDE_HOME)/.autodownloaded" -a -e "$(SDE_HOME)/sde" ]; then git submodule deinit --quiet -f $(SDE_HOME)/pinplay-tools; find $(SDE_HOME)/ -mindepth 1 -not -name 'pinplay-tools' -exec rm -rf {} \; 2> /dev/null || true ; fi
	$(_MSG) '[DISTCL] Capstone'
	$(_CMD) if [ -e "$(CAPSTONE_INSTALL)/.autodownloaded" ]; then git submodule deinit --quiet -f capstone; fi
	$(_MSG) '[DISTCL] DynamoRIO'
	$(_CMD) if [ -e "$(DYNAMORIO_INSTALL)/.autodownloaded" ]; then git submodule deinit --quiet -f dynamorio; fi
	$(_MSG) '[DISTCL] python_kit'
	$(_CMD) rm -rf python_kit
	$(_MSG) '[DISTCL] McPAT'
	$(_CMD) git submodule deinit --quiet -f mcpat
	$(_MSG) '[DISTCL] Xed'
	$(_CMD) git submodule deinit --quiet -f xed mbuild
	$(_CMD) rm -rf xed_kit
	$(_MSG) '[DISTCL] perf_event.h'
	$(_CMD) rm -f include/linux/perf_event.h

regress_quick: regress_unit regress_apps

empty_deps:
	$(_MSG) '[CLEAN ] deps'
	$(_CMD) find . -name \*.d -exec rm {} \;

package_deps:
	@BOOST_INCLUDE=$(BOOST_INCLUDE) ./tools/checkdependencies.py
