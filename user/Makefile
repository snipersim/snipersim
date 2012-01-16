SIM_ROOT ?= $(shell cd "$(CURDIR)/../" && pwd -P)

CFLAGS += -I $(SIM_ROOT)/common/misc

CLEAN=$(findstring clean,$(MAKECMDGOALS))

# Grab all c/c++ files from subdirs
SOURCES = $(wildcard *.c)

# Convert the .c and .cc's in to .o's
OBJECTS = $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(patsubst %.cc,%.o,$(SOURCES) ) ) )

TARGET=$(SIM_ROOT)/lib/libgraphite_user.a

# targets
all: $(TARGET)

$(TARGET): $(OBJECTS)
	ar rcs $@ $^

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<

ifneq ($(CLEAN),)
clean:
	-rm -f $(TARGET) $(OBJECTS) $(OBJECTS:%.o=%.d)
endif
