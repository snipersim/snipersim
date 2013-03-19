#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#ifdef USE_MPI
  #include <mpi.h>
#endif /* USE_MPI */
#ifdef _OPENMP
  #include <omp.h>
#endif /* _OPENMP */
#include <sim_api.h>

int read_slab_info() {
  /* This should read info from a file or something,
     but we fake it */
  return 16;
}

double process_slab(int snum)
{
  int i, j;
  double x;

  for (i = 0; i < 20; i++)
    for (j = 0; j < 100; j++)
      x += sqrt((i-j)*(i-j) / (sqrt((i*i) + (j*j)) + 1));

  return x;
}

void exit_on_error(char *message)
{
  fprintf(stderr, "%s\n", message);
#ifdef USE_MPI
  MPI_Finalize();
#endif
  exit(1);
}

int main(int argc, char **argv)
{
  int i, j, p, me, nprocs, num_threads, num_slabs, spp;
  int *my_slabs, *count;
  double x, sum;
#ifdef _OPENMP
  int np;
#endif /* _OPENMP */
#ifdef USE_MPI
  int namelen;
  char processor_name[MPI_MAX_PROCESSOR_NAME];
#endif /* USE_MPI */

#ifdef USE_MPI
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &me);
  MPI_Get_processor_name(processor_name, &namelen);
#else /* USE_MPI */
  nprocs = 1;
  me = 0;
#endif /* USE_MPI */

  SimRoiStart();

#ifdef _OPENMP
  num_threads = omp_get_max_threads();
#else /* _OPENMP */
  num_threads = 1;
#endif /* _OPENMP */
  printf("Process %d of %d", me, nprocs);
#ifdef USE_MPI
  printf(" running on %s", processor_name);
#endif /* USE_MPI */
#ifdef _OPENMP
  printf(" using OpenMP with %d threads",
    num_threads);
#endif /* _OPENMP */
  printf("\n");
  /* Master process reads slab data */
  if (!me) num_slabs = read_slab_info();
#ifdef USE_MPI
  if (MPI_Bcast(&num_slabs, 1, MPI_INT, 0,
      MPI_COMM_WORLD) != MPI_SUCCESS)
    exit_on_error("Error in MPI_Bcast()");
#endif /* USE_MPI */

  if (num_slabs < nprocs)
    exit_on_error("Number of slabs may not exceed number of processes");
  /* maximum number of slabs per process */
  spp = (int)ceil((double)num_slabs /
  (double)nprocs);
  if (!me) printf("No more than %d slabs will assigned to each process\n", spp);

  /* allocate list and count of slabs for each
    process */
  if (!(my_slabs = (int *)malloc(nprocs*spp*
  sizeof(int)))) {
  perror("my_slabs");
  exit(2);
  }
  if (!(count = (int *)malloc(nprocs*sizeof(int)))) {
    perror("count");
    exit(2);
  }
  /* initialize slab counts */
  for (p = 0; p < nprocs; p++) count[p] = 0;
  /* round robin assignment of slabs to processes
    for better potential
   * load balancing
   */
  for (i = j = p = 0; i < num_slabs; i++) {
    my_slabs[p*spp+j] = i;
    count[p]++;
    if (p == nprocs -1)
      p = 0, j++;
    else
      p++;
  }

  /* each process works on its own list of slabs,
     but OpenMP threads
   * divide up the slabs on each process because
     of OpenMP directive
   */
#pragma omp parallel for reduction(+: x)
  for (i = 0; i < count[me]; i++) {
    printf("%d: slab %d being processed", me,
      my_slabs[me*spp+i]);
#ifdef _OPENMP
    printf(" by thread %d", omp_get_thread_num());
#endif /* _OPENMP */
    printf("\n");
    x += process_slab(my_slabs[me*spp+i]);
  }

#ifdef USE_MPI
  if (MPI_Reduce(&x, &sum, 1, MPI_DOUBLE, MPI_SUM, 0,
      MPI_COMM_WORLD) != MPI_SUCCESS)
    exit_on_error("Error in MPI_Reduce()");
#else /* USE_MPI */
  sum = x;
#endif /* USE_MPI */

  if (!me) printf("Sum is %lg\n", sum);

  SimRoiEnd();

#ifdef USE_MPI
  printf("%d: Calling MPI_Finalize()\n", me);
  MPI_Finalize();
#endif /* USE_MPI */
  exit(0);
}
