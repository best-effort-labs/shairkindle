#include "daemon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
int raopd_lock_acquire(const char *lockpath) {
    int fd = open(lockpath, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;   /* held for process lifetime */
}
int raopd_pid_publish(const char *pidpath, pid_t pid) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp.%ld", pidpath, (long)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%ld\n", (long)pid);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, pidpath) != 0) { unlink(tmp); return -1; }
    return 0;
}
int raopd_pid_is_ours(const char *pidpath, const char *exe_basename) {
    FILE *f = fopen(pidpath, "r");
    if (!f) return 0;
    long pid = 0; int got = fscanf(f, "%ld", &pid); fclose(f);
    if (got != 1 || pid <= 0) return 0;
    if (kill((pid_t)pid, 0) != 0) return 0;                 /* not alive */
    char link[64], target[512];
    snprintf(link, sizeof link, "/proc/%ld/exe", pid);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n < 0) return 0;                                     /* can't verify -> not ours */
    target[n] = 0;
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strcmp(base, exe_basename) == 0;
}
