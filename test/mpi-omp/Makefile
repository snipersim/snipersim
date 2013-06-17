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
	OMP_NUM_THREADS=2 OMP_WAIT_POLICY=passive ../../run-sniper -c gainestown -n 4 --mpi --mpi-ranks=2 -- ./hybrid
