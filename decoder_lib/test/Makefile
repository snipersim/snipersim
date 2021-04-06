CC = gcc 
CFLAGS = -I.. -I../../pin_kit/extras/xed-intel64/include/xed -I../../capstone/include -I../../capstone/include/capstone
LFLAGS = -L../../xed/obj
LIBS = ../libdecoder.a ../../capstone/libcapstone.a -lstdc++ -lxed

.PHONY: all

all: test_x86 test_arm

%.o: %.cc
	$(CXX) -std=c++11 -c -o $@ $< $(CFLAGS)

test_x86: test_x86.o
	$(CXX) $(LFLAGS) -o $@ $^ $(LIBS)

test_arm: test_arm.o
	$(CXX) $(LFLAGS) -o $@ $^ $(LIBS)

.PHONY: clean

clean:
		rm -f *.o test_x86 test_arm

