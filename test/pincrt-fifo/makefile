# Builds the Pin tool (obj-intel64/fifo_check.so) and the application with
# Pin's own makefile infrastructure; "make native" builds the glibc twin.
#   make PIN_ROOT=/path/to/pin-kit
PIN_ROOT ?= ../../sde_kit/pinkit
CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config
include makefile.rules
include $(TOOLS_ROOT)/Config/makefile.default.rules

native: fifo_check_native
fifo_check_native: fifo_check.cpp
	$(CXX) -O0 -g -o $@ $< -lpthread

# DynamoRIO twin of the tool (make DR_ROOT=/path/to/DynamoRIO-Linux-*)
DR_ROOT ?= /opt/dynamorio
fifo_check_dr.so: fifo_check.cpp
	$(CXX) -shared -fPIC -DDR_CLIENT -DLINUX -DX86_64 -fno-exceptions -I$(DR_ROOT)/include -I$(DR_ROOT)/ext/include -o $@ $< \
	   -L$(DR_ROOT)/lib64/release -ldynamorio -L$(DR_ROOT)/ext/lib64/release -ldrwrap -ldrmgr
