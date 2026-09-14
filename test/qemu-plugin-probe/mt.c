/* Multi-threaded workload that exercises exactly the hazard:
 * worker threads spin (so they trap in the plugin callback and block),
 * while the main thread keeps calling clone() -- each pthread_create is a
 * clone(), and linux-user's clone path runs start_exclusive().           */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static volatile uint64_t sink;
static volatile int stop;

static void *spin(void *arg)
{
    uint64_t x = (uint64_t)(uintptr_t)arg;
    while (!stop) { for (int i = 0; i < 100000; i++) x = x * 6364136223846793005ULL + 1; }
    sink += x;
    return NULL;
}

static void *shortlived(void *arg) { return NULL; }

int main(int argc, char **argv)
{
    int nspin = argc > 1 ? atoi(argv[1]) : 3;
    int nclone = argc > 2 ? atoi(argv[2]) : 40;
    pthread_t t[16];

    for (int i = 0; i < nspin; i++) pthread_create(&t[i], NULL, spin, (void *)(uintptr_t)(i + 1));

    /* Repeated clone()/join while the spinners are busy inside callbacks. */
    for (int i = 0; i < nclone; i++) {
        pthread_t s;
        pthread_create(&s, NULL, shortlived, NULL);
        pthread_join(s, NULL);
        fprintf(stderr, "clone %d ok\n", i);
    }

    stop = 1;
    for (int i = 0; i < nspin; i++) pthread_join(t[i], NULL);
    printf("mt done sink=%llu\n", (unsigned long long)sink);
    return 0;
}
