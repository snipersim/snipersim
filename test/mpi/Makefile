include ../../config/buildconf.makefile

MPICC=mpicc
MPILD=mpicc
MPIRUN=mpirun

all: pi
	@echo "\nRun 'make run' to run the fft program in the simulator\n"

.c.o:
	$(MPICC) -c -o $@ $< $(GRAPHITE_CFLAGS)

pi: pi_mpi.o Makefile
	$(MPILD) -o pi pi_mpi.o -lm $(GRAPHITE_LDFLAGS)

run: pi
	../../run-sniper --mpi -n 4 -c gainestown -- mpirun -np 4 ./pi

clean:
	rm -f *.o pi sim.cfg sim.stats* *.log *.out *.jpg
