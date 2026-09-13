# Compiler and flags
# ?= so that CC/CXX from the environment or the top-level make are honoured;
# these were plain = and silently ignored the compiler the rest of the tree used.
CC ?= gcc
CXX ?= g++
CFLAGS += -Wall -Wextra -g
CXXFLAGS += -Wall -Wextra -g
# Emit dependency files during compilation so header edits trigger a rebuild;
# there was no header tracking here at all.
DEPFLAGS = -MMD -MP
LDFLAGS ?=

# Directories
SRC_DIR = ./src

INCLUDE_DIR = -I./include/mm
INCLUDE_DIR += -I./include/misc
INCLUDE_DIR += -I./include/
INCLUDE_DIR += -I./src
INCLUDE_DIR += -I../include

BUILD_DIR = ./build
OBJ_DIR = $(BUILD_DIR)/obj

# Source files (fix: quote the patterns so 'find' doesn't let the shell expand them)
C_SRC_FILES   = $(shell find $(SRC_DIR) -type f -name '*.c')
CPP_SRC_FILES = $(shell find $(SRC_DIR) -type f -name '*.cc')

# Object lists
C_OBJ_FILES   = $(C_SRC_FILES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CPP_OBJ_FILES = $(CPP_SRC_FILES:$(SRC_DIR)/%.cc=$(OBJ_DIR)/%.o)
OBJ_FILES     = $(C_OBJ_FILES) $(CPP_OBJ_FILES)

# Main binary
MAIN_BINARY = $(BUILD_DIR)/startup_mimicos

# Targets
.PHONY: all clean

all: $(MAIN_BINARY)

# Create necessary directories
$(OBJ_DIR):
	mkdir -p $@

# Ensure all object subdirectories are created (mkdir on demand in compile rules)
# (OBJ_SUBDIRS block kept as-is; it's harmless even if empty)
OBJ_SUBDIRS :=
$(OBJ_FILES): | $(OBJ_SUBDIRS)

$(OBJ_SUBDIRS):
	mkdir -p $@

# Ensure the top-level build directory exists before linking outputs
$(BUILD_DIR):
	mkdir -p $@

# Build C object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDE_DIR) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Build C++ object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cc | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(INCLUDE_DIR) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Link main binary (will include build/obj/virtuos/virtuos.o automatically)
$(MAIN_BINARY): $(OBJ_FILES) startup_mimicos.cc | $(BUILD_DIR)
	$(CXX) $(INCLUDE_DIR) $^ -o $@ -lrt

-include $(OBJ_FILES:.o=.d)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
