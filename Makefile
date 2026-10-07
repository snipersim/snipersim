# Sniper — `make` for people who type `make`.
#
# The build is CMake now.  This forwards to it so that existing habits, scripts
# and papers keep working:
#
#     make                 acquire dependencies, configure, build
#     make -j8             passed through
#     make clean           clean the build directory
#     make distclean       remove build directories and the fetched dependencies
#     make regress         run the regression suites, then report where the
#                          time went.  SNIPER_REGRESS_LOG=<file> keeps the
#                          per-check timings; testing/regress_report.py reads
#                          one back, so two runs can be compared.
#     make regress FAILFAST=ON
#                          stop at the first failing check and keep its work
#                          directory, for debugging that one failure
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
RISCV ?= ON
ARM ?= OFF
DYNAMORIO ?= OFF
QEMU ?= OFF
RISCV_IME ?= OFF   # QEMU riscv64 with the IME 0.9 patch, for the RISC-V/IME checks
BUILD_TYPE ?= Release
DYNAMORIO_DIR ?=
CMAKE_ARGS ?=

_deps_flags := --frontend $(FRONTEND)
_deps_flags += $(if $(filter ON,$(TORCH)),,--no-torch)
_deps_flags += $(if $(filter ON,$(MCPAT)),,--no-mcpat)
_deps_flags += $(if $(filter ON,$(ARM)),--arm,)
_deps_flags += $(if $(filter ON,$(DYNAMORIO)),--dynamorio,)
_deps_flags += $(if $(filter ON,$(QEMU)),--qemu,)
_deps_flags += $(if $(filter ON,$(RISCV_IME)),--riscv-ime,)

_cmake_flags := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
_cmake_flags += -DSNIPER_FRONTEND=$(FRONTEND)
_cmake_flags += -DSNIPER_ENABLE_TORCH=$(TORCH)
_cmake_flags += -DSNIPER_ENABLE_MCPAT=$(MCPAT)
_cmake_flags += -DSNIPER_ENABLE_RISCV=$(RISCV)
_cmake_flags += -DSNIPER_ENABLE_ARM=$(ARM)
_cmake_flags += -DSNIPER_ENABLE_DYNAMORIO=$(DYNAMORIO)
_cmake_flags += -DSNIPER_ENABLE_QEMU=$(QEMU)

# Locations that are not always where the build would guess.
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

.PHONY: all deps configure build clean distclean regress regress_scripting regress_configs regress_qemu regress_pacsim validate_pacsim regress_apps regress_virtuoso riscv-ime-check help

all: build

# The same -j the build was asked for: some dependencies ship as source and
# have to be compiled, and XED's mfile.py otherwise takes one job per core --
# eighty parallel compilers on a large host, from a build asked for eight.
deps:
	@$(DEPS) $(_deps_flags) $(_deps_jobs) fetch

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
_deps_jobs = $(if $(patsubst -j,,$(_jflag)),--jobs $(patsubst -j%,%,$(_jflag)),)

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

# Every suite runs, then the timing report, then the run fails if any suite did.
#
# As a prerequisite list this stopped at the first failing suite, which is both
# the moment you most want the rest of the picture and the moment the report
# would not have printed.  SNIPER_REGRESS_LOG is where the suites append their
# per-check timings; see testing/common/runlog.py.
REGRESS_SUITES = scripting configs qemu pacsim apps virtuoso

# FAILFAST=ON stops at the first failing check, keeps the work directory it ran
# in, and skips the remaining suites.  For debugging one failure, not for a
# regression pass: a pass is worth running for the whole picture, and the first
# failure is rarely the only thing worth seeing.
FAILFAST ?= OFF
_failfast := $(if $(filter ON,$(FAILFAST)),--failfast,)

# Pass SNIPER_REGRESS_LOG=<file> to keep the timings and compare two runs with
#   testing/regress_report.py <file>
regress:
	@log="$(SNIPER_REGRESS_LOG)"; keep=1; \
	if [ -z "$$log" ]; then \
	  log=$$(mktemp "$${TMPDIR:-/tmp}/sniper-regress-XXXXXX.jsonl"); keep=; \
	fi; \
	: > "$$log"; rc=0; export SNIPER_REGRESS_LOG="$$log"; \
	for s in $(REGRESS_SUITES); do \
	  $(SIM_ROOT)/testing/test_$$s.py $(_failfast) || rc=$$?; \
	  if [ $$rc -ne 0 ] && [ -n "$(_failfast)" ]; then \
	    echo "FAILFAST: stopping; $$s failed and the remaining suites are skipped"; \
	    break; \
	  fi; \
	done; \
	$(SIM_ROOT)/testing/regress_report.py "$$log" || true; \
	if [ -z "$$keep" ]; then rm -f "$$log"; \
	else echo "timings kept in $$log"; fi; \
	exit $$rc

regress_scripting:
	@$(SIM_ROOT)/testing/test_scripting.py

regress_configs:
	@$(SIM_ROOT)/testing/test_configs.py

# Skips itself with a reason unless QEMU and the frontend are built (QEMU=ON).
regress_qemu:
	@$(SIM_ROOT)/testing/test_qemu.py

# The workloads under test/.  Each has always had a `make run` target and
# nothing ran them; each now also has a check on what came out, because `make
# run` only ever reported whether the simulator crashed.
regress_apps:
	@$(SIM_ROOT)/testing/test_apps.py

# The Virtuoso v2 integration: prove the additions changed nothing else, and
# check each landed feature's counters.  Skips with a reason until the baselines
# are blessed, which is deliberate -- they are host-specific, so they are made
# locally rather than committed:
#
#     testing/test_virtuoso.py --bless
#
regress_virtuoso:
	@$(SIM_ROOT)/testing/test_virtuoso.py

# Sampled simulation, in two sizes.
#
# Both run the same three region shapes -- barrier-delimited, loop-delimited and
# task-queue -- and both check the same three things: that the two runs executed
# the same program, that sampling was faster, and that its estimate agreed with
# the full run.  They differ only in workload size.
#
# regress_pacsim is the small one, sized so that a regression pass still notices
# if sampling stops helping or starts disagreeing.  validate_pacsim is the study
# version, whose numbers are meant to be quotable; it simulates a full detailed
# baseline three times over and costs accordingly.
#
# Both skip themselves, with a reason, if the recorder has no Pac-Sim support.
regress_pacsim:
	@$(SIM_ROOT)/testing/test_pacsim.py --profile quick

validate_pacsim:
	@$(SIM_ROOT)/testing/test_pacsim.py --profile full

# The header comment is the help text: print it up to the first line that is not
# a comment, so the two cannot drift apart.
help:
	@sed -n '/^#/!q; s/^#\{1\} \{0,1\}//p' $(lastword $(MAKEFILE_LIST))

# The RISC-V / IME 0.9 chain, end to end: specification -> generated decoder,
# QEMU with the IME patch, a workload built with a stock clang, the harness.
riscv-ime-check:
	@tools/riscv/ime-check.sh
.PHONY: riscv-ime-check
