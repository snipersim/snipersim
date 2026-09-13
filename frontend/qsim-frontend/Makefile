SIM_ROOT ?= $(shell readlink -f "$(CURDIR)/../..")

# QSim (the QEMU-based frontend library).  Not fetched by the build; point
# QSIM_HOME at an existing checkout, or leave it at the default location.
QSIM_HOME ?= $(SIM_ROOT)/qsim

default: qsim-frontend

# Pulls in the same CPPFLAGS, CXXFLAGS and LD_FLAGS as the rest of the tree --
# in particular the C++ standard and -D_GLIBCXX_USE_CXX11_ABI, which have to
# match libsift.a and libcarbon_sim.a or the link fails on mangled names.
include $(SIM_ROOT)/common/Makefile.common

CPPFLAGS += -I.. -I. -I$(QSIM_HOME)/include -I$(QSIM_HOME)/distorm/
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
