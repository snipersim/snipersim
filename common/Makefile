SIM_ROOT ?= $(shell readlink -f "$(CURDIR)/../")

CLEAN=$(findstring clean,$(MAKECMDGOALS))

SRC_DIRECTORIES = $(DIRECTORIES)

# Convert the .c and .cc's in to .o's
OBJECTS = $(LIBCARBON_OBJECTS)

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
	$(_MSG) '[AR    ]' $(subst $(shell readlink -f $(SIM_ROOT))/,,$(shell readlink -f $@))
	$(_CMD) ar rcs $@ $^

ifneq ($(CLEAN),clean)
-include $(patsubst %.cpp,%.d,$(patsubst %.c,%.d,$(patsubst %.cc,%.d,$(LIBCARBON_SOURCES))))
endif

ifneq ($(CLEAN),)
clean:
	@rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
