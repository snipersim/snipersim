include ../../config/buildconf.makefile

CXX=$(SNIPER_CXX)

true: true.cc Makefile
	$(CXX) -o true true.cc $(SNIPER_LDFLAGS) $(SNIPER_CFLAGS)

run: true
	../../run-sniper -n 1 -- ./true

clean:
	rm -f true sim.cfg sim.stats* *.log *.out *.jpg
