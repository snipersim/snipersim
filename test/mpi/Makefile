TARGET=pi
include ../shared/Makefile.shared

MPICC=mpicc
MPILD=mpicc
MPIRUN=mpirun

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
