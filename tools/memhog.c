// DDR bandwidth hog for the 3D-freeze test: N threads memcpy between two 64 MB buffers forever; prints GB/s every 10 s.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SZ (64u << 20)
static volatile unsigned long long bytes;

static void *run(void *arg) {
    char *a = malloc(SZ), *b = malloc(SZ);
    if (!a || !b) { perror("malloc"); exit(1); }
    memset(a, 1, SZ); memset(b, 2, SZ);
    for (;;) { memcpy(b, a, SZ); __asm__ volatile("" ::"r"(b) : "memory"); memcpy(a, b, SZ); __asm__ volatile("" ::"r"(a) : "memory"); __atomic_fetch_add(&bytes, 4ull * SZ, __ATOMIC_RELAXED); } // read+write per copy
    return arg;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3;
    pthread_t t;
    for (int i = 0; i < n; i++) pthread_create(&t, 0, run, 0);
    unsigned long long last = 0;
    for (int s = 10;; s += 10) {
        sleep(10);
        unsigned long long now = bytes;
        printf("memhog: %d s, %.2f GB/s\n", s, (now - last) / 10.0 / 1e9); fflush(stdout);
        last = now;
    }
}
