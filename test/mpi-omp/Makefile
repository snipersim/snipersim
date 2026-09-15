TARGET=hybrid
include ../shared/Makefile.shared

# MPI comes from deps/sniper-deps --mpi, which builds MPICH into mpich/ at the
# top level.  No system MPI is assumed -- this machine had none, and installing
# one needs root -- but a system MPI is still used if the dependency is absent,
# so anyone who already has one keeps working.
MPI_HOME ?= $(SNIPER_ROOT)/mpich
MPICC=$(if $(wildcard $(MPI_HOME)/bin/mpicc),$(MPI_HOME)/bin/mpicc,mpicc)
MPILD=$(MPICC)
MPIRUN=$(if $(wildcard $(MPI_HOME)/bin/mpirun),$(MPI_HOME)/bin/mpirun,mpirun)

MPI_CFLAGS=$(GRAPHITE_CFLAGS) -fopenmp -DUSE_MPI
MPI_LDFLAGS=$(filter-out -static,$(GRAPHITE_LDFLAGS)) -fopenmp

.c.o:
	$(MPICC) -c -o $@ $< $(MPI_CFLAGS)

hybrid: hybrid.o Makefile
	$(MPILD) -o hybrid hybrid.o -lm $(MPI_LDFLAGS)


run_$(TARGET):
	OMP_NUM_THREADS=2 OMP_WAIT_POLICY=passive ../../run-sniper -c gainestown -n 4 --mpi --mpi-ranks=2 -- ./hybrid
