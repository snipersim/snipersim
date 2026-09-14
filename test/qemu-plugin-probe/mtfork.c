/* Spinner threads (which block inside plugin callbacks) + real fork() from
 * main.  fork() is the linux-user path that DOES call start_exclusive().   */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile uint64_t sink;
static volatile int stop;

static void *spin(void *arg)
{
    uint64_t x = (uint64_t)(uintptr_t)arg;
    while (!stop) { for (int i = 0; i < 10000; i++) x = x * 6364136223846793005ULL + 1; }
    sink += x;
    return NULL;
}

int main(int argc, char **argv)
{
    int nspin  = argc > 1 ? atoi(argv[1]) : 3;
    int nforks = argc > 2 ? atoi(argv[2]) : 10;
    pthread_t t[16];

    for (int i = 0; i < nspin; i++)
        pthread_create(&t[i], NULL, spin, (void *)(uintptr_t)(i + 1));

    usleep(50000);  /* let the spinners get going and start trapping */

    for (int i = 0; i < nforks; i++) {
        fprintf(stderr, "fork %d ...\n", i);
        pid_t p = fork();
        if (p == 0) { _exit(0); }
        if (p < 0) { perror("fork"); break; }
        waitpid(p, NULL, 0);
        fprintf(stderr, "fork %d ok\n", i);
    }

    stop = 1;
    for (int i = 0; i < nspin; i++) pthread_join(t[i], NULL);
    printf("mtfork done sink=%llu\n", (unsigned long long)sink);
    return 0;
}
