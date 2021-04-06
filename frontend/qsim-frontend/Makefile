SIM_ROOT ?= $(CURDIR)/../..
QSIM_PREFIX = $(SIM_ROOT)/qsim

default: qsim-frontend

CXX ?= g++
CPPFLAGS = -I.. -I. -I../../sift -I$(QSIM_PREFIX)/include -I$(QSIM_PREFIX)/distorm/ 
CXXFLAGS = -g -c -Wall -Wextra -Wcast-align -Wno-unused-parameter -Wno-unknown-pragmas -std=c++11 -fno-strict-aliasing
LINKER?=${CXX}
#CXXFLAGS += -std=c++0x -Wall -Wno-unknown-pragmas $(DBG) $(OPT_CFLAGS) $(TOOL_CXXFLAGS) -I.. -I../../common/misc -I../../sift
LDFLAGS += -L.. -L../../sift -L ../../lib -L$(QSIM_PREFIX)/lib -lqsim -ldl -lsift -lcarbon_sim -lz -lrt $(QSIM_PREFIX)/distorm/distorm64.a -lcapstone

qsim-frontend: qsim-frontend.o bbv_count.o ../../sift/libsift.a 
	$(CXX) -o $@ qsim-frontend.o bbv_count.o $(LDFLAGS)

qsim-frontend.o: qsim_frontend.cc ../frontend_options.h ../frontend_syscall.h ../frontend_threads.h ../frontend_control.h ../frontend.h ../frontend-inl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

#qsim_threads.o: ../frontend_threads.cc ../frontend_threads.h
#	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

bbv_count.o: ../bbv_count.cc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

include $(SIM_ROOT)/common/Makefile.common

clean:
ifeq ($(SHOW_COMPILE),)
	@rm -f *.o *.d qsim-frontend
else
	rm -f *.o *.d qsim-frontend
endif

ifneq ($(CLEAN),clean)
-include $(patsubst %.cc,%.d,$(SOURCES))
endif
