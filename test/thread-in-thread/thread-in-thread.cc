#include <pthread.h>

#define THREAD_NUM 5

void *spin(void* ptr)
{
    for (int i = 0; i < 100; i++)
        ;
}

void *thread_in_thread(void *ptr)
{
    pthread_t threads[THREAD_NUM];

    for (int i = 0; i < THREAD_NUM; i++)
        pthread_create(&threads[i], NULL, *spin, NULL);

    for (int i = 0; i < THREAD_NUM; i++)
        pthread_join(threads[i], NULL);
}

int main()
{
    pthread_t threads[THREAD_NUM];
    
    for (int i = 0; i < THREAD_NUM; i++)
        pthread_create(&threads[i], NULL, *thread_in_thread, NULL);

    for (int i = 0; i < THREAD_NUM; i++)
        pthread_join(threads[i], NULL);

    return 0;
}
