include ../../config/buildconf.makefile

MPICC=mpicc
MPILD=mpicc
MPIRUN=mpirun

MPI_CFLAGS=$(GRAPHITE_CFLAGS)
MPI_LDFLAGS=$(filter-out -static,$(GRAPHITE_LDFLAGS))

all: pi
	@echo "\nRun 'make run' to run the fft program in the simulator\n"

.c.o:
	$(MPICC) -c -o $@ $< $(MPI_CFLAGS)

pi: pi_mpi.o Makefile
	$(MPILD) -o pi pi_mpi.o -lm $(MPI_LDFLAGS)

run: pi
	../../run-sniper --mpi -n 4 -c gainestown -- mpirun -np 4 ./pi

clean:
	rm -f *.o pi sim.cfg sim.stats* *.log *.out *.jpg
