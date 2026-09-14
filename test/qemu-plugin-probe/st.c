/* Single-threaded equivalent: same work, one thread, no clone. */
#include <stdio.h>
#include <stdint.h>
int main(void)
{
    uint64_t x = 1;
    for (int r = 0; r < 40; r++)
        for (int i = 0; i < 200000; i++) x = x * 6364136223846793005ULL + 1;
    printf("st done x=%llu\n", (unsigned long long)x);
    return 0;
}
