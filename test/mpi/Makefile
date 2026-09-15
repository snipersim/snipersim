TARGET=pi
include ../shared/Makefile.shared

# MPI comes from deps/sniper-deps --mpi, which builds MPICH into mpich/ at the
# top level.  No system MPI is assumed -- this machine had none, and installing
# one needs root -- but a system MPI is still used if the dependency is absent,
# so anyone who already has one keeps working.
MPI_HOME ?= $(SNIPER_ROOT)/mpich
MPICC=$(if $(wildcard $(MPI_HOME)/bin/mpicc),$(MPI_HOME)/bin/mpicc,mpicc)
MPILD=$(MPICC)
MPIRUN=$(if $(wildcard $(MPI_HOME)/bin/mpirun),$(MPI_HOME)/bin/mpirun,mpirun)

MPI_CFLAGS=$(GRAPHITE_CFLAGS)
MPI_LDFLAGS=$(filter-out -static,$(GRAPHITE_LDFLAGS))

.c.o:
	$(MPICC) -c -o $@ $< $(MPI_CFLAGS)

pi: pi_mpi.o Makefile
	$(MPILD) -o pi pi_mpi.o -lm $(MPI_LDFLAGS)


run_$(TARGET):
	../../run-sniper --mpi -n 4 -c gainestown -- ./pi

run_mpirun:
	../../run-sniper --mpi --mpi-ranks=2 --mpi-exec="mpirun -genv ENVNAME VALUE" -n 4 -c gainestown -- ./pi
