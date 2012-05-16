include ../../config/buildconf.makefile

CXX=$(GRAPHITE_CXX)

true: true.cc Makefile
	$(CXX) -o true true.cc $(GRAPHITE_LDFLAGS) $(GRAPHITE_CFLAGS)

clean:
	rm -f true sim.cfg sim.stats* *.log *.out *.jpg
