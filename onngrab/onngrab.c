// libonngrab.so: picture-in-picture controller routing for games on the onn box.
//
// Loaded into a game with LD_PRELOAD (run_apk.sh, run_sm64.sh). It notes which /dev/input/event* files the game opens
// (SDL's joystick code reads the pad through evdev). OnnShell then sends:
//   SIGUSR1: grab those devices (EVIOCGRAB). Only this game's fds see the pad; Android, and so the app playing full
//            screen behind the corner window, sees nothing from it. The TV remote is a different device and still works.
//   SIGUSR2: release them (back to full screen, or the corner window closed).
// While grabbed, OnnShell cannot see the pad either, so two buttons are watched here, on the events the game reads:
//   PS (BTN_MODE) press: release the grab and create /data/local/tmp/onn-pip-exit (OnnShell brings the game back).
//   Share (BTN_SELECT) press: create /data/local/tmp/onn-pip-audio (OnnShell swaps game audio on/off).
// Everything else passes through untouched; with no signal nothing ever changes, so a game without OnnShell runs as before.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAXFD 256
static volatile sig_atomic_t evfd[MAXFD]; // 1: the fd is an evdev device the game opened
static volatile sig_atomic_t grabbed;

static void mark(int fd, const char *path) {
    if (fd >= 0 && fd < MAXFD) evfd[fd] = path != NULL && strncmp(path, "/dev/input/event", 16) == 0;
}

// ioctl and raw syscalls only: both run inside a signal handler
static void setgrab(int on) {
    for (int i = 0; i < MAXFD; i++)
        if (evfd[i]) ioctl(i, EVIOCGRAB, on ? (void *)1 : (void *)0);
    grabbed = on;
}

static void flag(const char *path) {
    int f = (int)syscall(__NR_openat, AT_FDCWD, path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (f >= 0) syscall(__NR_close, f);
}

static void on_signal(int sig) { int e = errno; setgrab(sig == SIGUSR1); errno = e; }

__attribute__((constructor)) static void init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}

#define REAL(name) static __typeof__(name) *real_##name; if (!real_##name) real_##name = (__typeof__(name) *)dlsym(RTLD_NEXT, #name)

static mode_t mode_arg(int flags, va_list ap) { return (flags & (O_CREAT | O_TMPFILE)) ? (mode_t)va_arg(ap, int) : 0; }

int open(const char *path, int flags, ...) {
    REAL(open); va_list ap; va_start(ap, flags); mode_t m = mode_arg(flags, ap); va_end(ap);
    int fd = real_open(path, flags, m); mark(fd, path); return fd;
}
int open64(const char *path, int flags, ...) {
    REAL(open64); va_list ap; va_start(ap, flags); mode_t m = mode_arg(flags, ap); va_end(ap);
    int fd = real_open64(path, flags, m); mark(fd, path); return fd;
}
int openat(int dir, const char *path, int flags, ...) {
    REAL(openat); va_list ap; va_start(ap, flags); mode_t m = mode_arg(flags, ap); va_end(ap);
    int fd = real_openat(dir, path, flags, m); mark(fd, path); return fd;
}
// FORTIFY builds call these instead of open/openat when the flags are known at compile time
int __open_2(const char *path, int flags) { REAL(__open_2); int fd = real___open_2(path, flags); mark(fd, path); return fd; }
int __openat_2(int dir, const char *path, int flags) { REAL(__openat_2); int fd = real___openat_2(dir, path, flags); mark(fd, path); return fd; }

int close(int fd) {
    REAL(close);
    if (fd >= 0 && fd < MAXFD && evfd[fd]) { if (grabbed) ioctl(fd, EVIOCGRAB, (void *)0); evfd[fd] = 0; }
    return real_close(fd);
}

static void watch(int fd, const void *buf, ssize_t n) {
    if (!grabbed || fd < 0 || fd >= MAXFD || !evfd[fd] || n < (ssize_t)sizeof(struct input_event)) return;
    const struct input_event *ev = buf;
    for (ssize_t i = 0; i < n / (ssize_t)sizeof *ev; i++) {
        if (ev[i].type != EV_KEY || ev[i].value != 1) continue;
        if (ev[i].code == BTN_MODE) { setgrab(0); flag("/data/local/tmp/onn-pip-exit"); }
        else if (ev[i].code == BTN_SELECT) flag("/data/local/tmp/onn-pip-audio");
    }
}

ssize_t read(int fd, void *buf, size_t count) {
    REAL(read); ssize_t n = real_read(fd, buf, count); watch(fd, buf, n); return n;
}
ssize_t __read_chk(int fd, void *buf, size_t count, size_t size) {
    REAL(__read_chk); ssize_t n = real___read_chk(fd, buf, count, size); watch(fd, buf, n); return n;
}
