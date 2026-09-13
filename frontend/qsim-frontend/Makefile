SIM_ROOT ?= $(shell readlink -f "$(CURDIR)/../..")

# QSim (the QEMU-based frontend library).  Not fetched by the build; point
# QSIM_HOME at an existing checkout, or leave it at the default location.
#
# NOTE: qsim_frontend.cc does not currently compile against frontend/.  It was
# last touched 2021-04-15; the frontend framework moved on 2022-10-25 and only
# pin_frontend.cc was updated with it.  Three things have to change:
#
#   1. The framework constructs the frontend with `new T()` (frontend.tcc:23),
#      so QsimFrontend needs a default constructor.  It has only
#      QsimFrontend(FrontendISA), which nothing calls -- so the ISA selection is
#      orphaned and someone has to decide where it now comes from.  This is the
#      one part that is a design question rather than a mechanical fix.
#   2. __sendInstructionSpecialized is defined on Frontend<QsimFrontend>::,
#      which no longer declares it.  pin_frontend.cc defines it on the derived
#      class (PinFrontend::__sendInstructionSpecialized); qsim should match.
#   3. The sendInstruction call at qsim_frontend.cc:351 is not in scope.
#
# The headers also moved: current QSim keeps qsim.h, qsim-load.h and qsim-regs.h
# at the repository root, not under include/, which is where CPPFLAGS looks.
#
# Everything below -- flags, include paths, the shared-flag inheritance -- has
# been checked; the source is what needs porting, and it needs someone who can
# run a QSim build to validate the result.
QSIM_HOME ?= $(SIM_ROOT)/qsim

default: qsim-frontend

# Pulls in the same CPPFLAGS, CXXFLAGS and LD_FLAGS as the rest of the tree --
# in particular the C++ standard and -D_GLIBCXX_USE_CXX11_ABI, which have to
# match libsift.a and libcarbon_sim.a or the link fails on mangled names.
include $(SIM_ROOT)/common/Makefile.common

# Both: older QSim installed headers under include/, current QSim keeps them at
# the repository root.
CPPFLAGS += -I.. -I. -I$(QSIM_HOME) -I$(QSIM_HOME)/include -I$(QSIM_HOME)/distorm/
LD_FLAGS += -L$(QSIM_HOME)/lib
QSIM_LIBS = -lqsim $(QSIM_HOME)/distorm/distorm64.a -lcapstone

OBJECTS = qsim_frontend.o bbv_count.o

# Fail with something useful rather than a screenful of missing-header errors.
ifeq ($(wildcard $(QSIM_HOME)/include),)
$(OBJECTS): qsim-missing
.PHONY: qsim-missing
qsim-missing:
	@echo >&2
	@echo "        QSim not found at $(QSIM_HOME)." >&2
	@echo "        Clone it there, or build with QSIM_HOME=/path/to/qsim." >&2
	@echo >&2
	@false
endif

qsim-frontend: $(OBJECTS) $(SIM_ROOT)/sift/libsift.a $(SIM_ROOT)/lib/libcarbon_sim.a
	$(_MSG) '[LD    ]' $(subst $(SIM_ROOT_ABS)/,,$(abspath $@))
	$(_CMD) $(CXX) -o $@ $(OBJECTS) $(LD_FLAGS) $(QSIM_LIBS) $(LD_LIBS)

bbv_count.o: ../bbv_count.cc
	$(_MSG) '[CXX   ]' $(subst $(SIM_ROOT_ABS)/,,$(abspath $@))
	$(_CMD) $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	@rm -f *.o *.d qsim-frontend

-include $(OBJECTS:.o=.d)
