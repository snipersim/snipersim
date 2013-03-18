TARGET=hybrid
include ../shared/Makefile.shared

MPICC=mpicc
MPILD=mpicc
MPIRUN=mpirun

MPI_CFLAGS=$(GRAPHITE_CFLAGS) -fopenmp -DUSE_MPI
MPI_LDFLAGS=$(filter-out -static,$(GRAPHITE_LDFLAGS)) -fopenmp

.c.o:
	$(MPICC) -c -o $@ $< $(MPI_CFLAGS)

hybrid: hybrid.o Makefile
	$(MPILD) -o hybrid hybrid.o -lm $(MPI_LDFLAGS)


run_$(TARGET):
	../../run-sniper --mpi -n 2 -c gainestown --cache-only -- mpirun -np 2 ./hybrid
