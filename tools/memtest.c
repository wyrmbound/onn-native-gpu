// Memory pattern-vs-traffic probe for the watchdog death (2026-11-08).
//   memtest slow <MB> <pause_ms>   grab <MB> in 64 MB pieces, then walk them forever, touching one 64 MB piece
//                                  per step (write then read-sum) and sleeping <pause_ms> between steps. Low traffic,
//                                  wide footprint. Prints MB/s every 10 s.
//   memtest shield <MB>            grab <MB> in 64 MB pieces, touch every page once, then sleep forever. Launch the
//                                  game while it holds. Kill it to release.
// Pieces are mmap'd so the kernel hands out real pages; nothing is freed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PIECE (64u << 20)

static char *grab(void) {
    char *p = mmap(0, PIECE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    memset(p, 0x5a, PIECE); // fault every page in now
    return p;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: memtest slow <MB> <pause_ms> | memtest shield <MB>\n"); return 2; }
    int n = atoi(argv[2]) / 64; if (n < 1) n = 1;
    char **pieces = calloc(n, sizeof *pieces);
    for (int i = 0; i < n; i++) pieces[i] = grab();
    printf("memtest: holding %d MB in %d pieces\n", n * 64, n); fflush(stdout);
    if (!strcmp(argv[1], "shield")) { for (;;) pause(); }
    int pause_ms = argc > 3 ? atoi(argv[3]) : 1000;
    unsigned long long bytes = 0, last = 0; time_t t0 = time(0), tl = t0;
    for (unsigned step = 0;; step++) {
        char *p = pieces[step % n];
        memset(p, (char)step, PIECE);
        volatile unsigned long long sum = 0;
        for (size_t i = 0; i < PIECE; i += 4096) sum += p[i]; // one read per page
        bytes += PIECE + PIECE / 4096;
        usleep(pause_ms * 1000);
        time_t now = time(0);
        if (now - tl >= 10) { printf("memtest: %ld s, %.1f MB/s, step %u\n", (long)(now - t0), (bytes - last) / (double)(now - tl) / 1e6, step); fflush(stdout); last = bytes; tl = now; }
    }
}
