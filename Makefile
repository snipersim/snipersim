# Sniper — `make` for people who type `make`.
#
# The build is CMake now.  This forwards to it so that existing habits, scripts
# and papers keep working:
#
#     make                 acquire dependencies, configure, build
#     make -j8             passed through
#     make clean           clean the build directory
#     make distclean       remove build directories and the fetched dependencies
#     make regress         run the regression suites
#     make deps            acquire dependencies only
#     make configure       configure only
#
# Anything more than that is better said directly:
#
#     cmake --preset x86-sde-debug && cmake --build --preset x86-sde-debug
#     cmake --list-presets
#
# and a build directory is selected at run time with
#
#     ./run-sniper --build-dir=build/x86-sde-debug ...
#
# The three phases -- acquire, configure, compile -- are separate commands on
# purpose.  Fusing them is what let the old build download 1.1 GB before the
# check that was supposed to prevent it had finished.

SIM_ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD_DIR ?= $(SIM_ROOT)/build/default

CMAKE ?= cmake
DEPS := $(SIM_ROOT)/deps/sniper-deps

# Feature selection, forwarded to both the dependency fetcher and CMake so the
# two cannot disagree about what this build needs.
FRONTEND ?= sde
TORCH ?= ON
MCPAT ?= ON
RISCV ?= OFF
ARM ?= OFF
DYNAMORIO ?= OFF
BUILD_TYPE ?= Release
RV8_HOME ?=
DYNAMORIO_DIR ?=
CMAKE_ARGS ?=

_deps_flags := --frontend $(FRONTEND)
_deps_flags += $(if $(filter ON,$(TORCH)),,--no-torch)
_deps_flags += $(if $(filter ON,$(MCPAT)),,--no-mcpat)
_deps_flags += $(if $(filter ON,$(RISCV)),--riscv,)
_deps_flags += $(if $(filter ON,$(ARM)),--arm,)
_deps_flags += $(if $(filter ON,$(DYNAMORIO)),--dynamorio,)

_cmake_flags := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
_cmake_flags += -DSNIPER_FRONTEND=$(FRONTEND)
_cmake_flags += -DSNIPER_ENABLE_TORCH=$(TORCH)
_cmake_flags += -DSNIPER_ENABLE_MCPAT=$(MCPAT)
_cmake_flags += -DSNIPER_ENABLE_RISCV=$(RISCV)
_cmake_flags += -DSNIPER_ENABLE_ARM=$(ARM)
_cmake_flags += -DSNIPER_ENABLE_DYNAMORIO=$(DYNAMORIO)

# Locations that are not always where the build would guess.
_cmake_flags += $(if $(RV8_HOME),-DSNIPER_RV8_HOME=$(RV8_HOME),)
_cmake_flags += $(if $(DYNAMORIO_DIR),-DDynamoRIO_DIR=$(DYNAMORIO_DIR),)

# Escape hatch: anything else, straight through.  `make CMAKE_ARGS=-DFoo=Bar`.
_cmake_flags += $(CMAKE_ARGS)

# Ninja when it is available; CMake's default generator otherwise.
#
# Only for a directory that has not been configured yet.  CMake refuses to
# change generator under an existing cache, so passing -G unconditionally turned
# "ninja has since been installed" into a hard error on every build directory
# that predated it.  Reconfiguring an existing directory keeps whatever it was
# built with; to switch, delete it.
_generator = $(if $(wildcard $(BUILD_DIR)/CMakeCache.txt),,\
             $(if $(shell command -v ninja 2>/dev/null),-G Ninja,))

.PHONY: all deps configure build clean distclean regress regress_scripting regress_configs help

all: build

deps:
	@$(DEPS) $(_deps_flags) fetch

configure: deps
	@$(CMAKE) -S $(SIM_ROOT) -B $(BUILD_DIR) $(_generator) $(_cmake_flags) \
	          -DSNIPER_LINK_INTO_TREE=ON

# Parallelism.
#
# GNU Make advertises its jobserver in MAKEFLAGS but only hands the descriptors
# to recipes it considers recursive.  This one is not, so ninja read the
# advertisement, failed to open the descriptors, printed
#
#     ninja: error: Could not initialize jobserver: Invalid file descriptors
#
# and then quietly built at its own default width -- an alarming message and a
# -j that was not the one asked for.  Translate -jN into an explicit --parallel
# and take the jobserver back out of the environment instead.
#
# Deferred (=) rather than immediate (:=): make appends -j and the jobserver to
# MAKEFLAGS after the makefile has been read, so an immediate assignment sees
# neither.
_jflag = $(filter -j%,$(MAKEFLAGS))
_parallel = --parallel $(patsubst -j%,%,$(_jflag))

# Depends on configure rather than on the cache file: re-running CMake on an
# already-configured directory costs about a second and re-runs nothing else,
# and it avoids a recursive $(MAKE) that would pick up the wrong makefile.
build: configure
	@MAKEFLAGS= MFLAGS= $(CMAKE) --build $(BUILD_DIR) $(_parallel)

clean:
	@if [ -d "$(BUILD_DIR)" ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	@rm -rf $(SIM_ROOT)/build $(SIM_ROOT)/lib/sniper
	@echo "Build directories removed."
	@echo "Fetched dependencies live in a shared cache and are left alone;"
	@echo "  $(DEPS) cache      shows where"
	@echo "  $(DEPS) gc         removes versions no manifest names"

regress: regress_scripting regress_configs

regress_scripting:
	@$(SIM_ROOT)/testing/test_scripting.py

regress_configs:
	@$(SIM_ROOT)/testing/test_configs.py

# The header comment is the help text: print it up to the first line that is not
# a comment, so the two cannot drift apart.
help:
	@sed -n '/^#/!q; s/^#\{1\} \{0,1\}//p' $(lastword $(MAKEFILE_LIST))
