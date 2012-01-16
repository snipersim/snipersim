SIM_ROOT ?= $(shell cd "$(CURDIR)/../" && pwd -P)

CLEAN=$(findstring clean,$(MAKECMDGOALS))

SRC_DIRECTORIES = $(DIRECTORIES) \
			 $(SIM_ROOT)/common/core/memory_subsystem/pr_l1_pr_l2_dram_directory_msi \
			 $(SIM_ROOT)/common/core/memory_subsystem/parametric_dram_directory_msi

# Grab all c/c++ files from subdirs
SOURCES = $(foreach dir,$(SRC_DIRECTORIES),$(wildcard $(dir)/*.cc)) \
	$(wildcard $(SIM_ROOT)/common/config/*.cpp)

# Convert the .c and .cc's in to .o's
OBJECTS = $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(patsubst %.cc,%.o,$(SOURCES) ) ) )

TARGET=$(SIM_ROOT)/lib/libcarbon_sim.a

# targets
all: $(TARGET)

# This include must be here
#  - The above targets need to be the default ones.  Makefile.common's would override it
#  - The clean command below must be overwritten by this Makefile to correctly clean 'common'
include $(SIM_ROOT)/common/Makefile.common

# This needs to be below the above include file
#  - OBJECTS depends on DIRECTORIES which is in the Makefile.common make file
#  - Therefore, we need to include the other makefile first before setting this target
$(TARGET): $(OBJECTS)
ifeq ($(SHOW_COMPILE),)
	@echo '[AR    ]' $(subst $(abspath $(SIM_ROOT))/,,$(abspath $@))
	@ar rcs $@ $^
else
	ar rcs $@ $^
endif

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
