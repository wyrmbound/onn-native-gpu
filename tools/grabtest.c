// grabtest /dev/input/eventN: tries EVIOCGRAB and releases it. Prints "free" or "busy" (someone else holds the grab).
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: grabtest /dev/input/eventN\n"); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, EVIOCGRAB, (void *)1) == 0) { ioctl(fd, EVIOCGRAB, (void *)0); puts("free"); }
    else puts(errno == EBUSY ? "busy" : "error");
    close(fd);
    return 0;
}
