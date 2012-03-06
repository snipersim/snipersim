SOURCES=sift_reader.cc sift_writer.cc sift_utils.cc zfstream.cc
OBJECTS=$(patsubst %.cc,%.o,$(SOURCES))
TARGET=libsift.a

all : $(TARGET) siftdump recorder

.PHONY : recorder

include ../common/Makefile.common

XED_HOME=$(PIN_HOME)/extras/xed2-intel64

CXXFLAGS=-O3 -fPIC -std=c++0x -I$(XED_HOME)/include

%.o : %.cc *.h Makefile
	$(CXX) -c -o $@ $< $(CXXFLAGS)

$(TARGET) : $(OBJECTS)
	$(AR) rcs $@ $^

siftdump : siftdump.o $(TARGET)
	$(CXX) -o $@ $^ -L$(XED_HOME)/lib -L. -lsift -lxed -lz

recorder :
	make -C recorder

clean :
	rm -f *.o $(TARGET) siftdump
	make -C recorder clean
