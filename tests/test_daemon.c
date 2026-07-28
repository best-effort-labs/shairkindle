#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "daemon.h"
int main(void) {
    const char *lock = "/tmp/raopd_test.lock";
    const char *pidf = "/tmp/raopd_test.pid";
    int fd = raopd_lock_acquire(lock);
    assert(fd >= 0);
    assert(raopd_lock_acquire(lock) == -1);          /* second holder blocked */
    assert(raopd_pid_publish(pidf, getpid()) == 0);
    /* our own pid, but exe basename is the test binary, not "raopd" */
    assert(raopd_pid_is_ours(pidf, "definitely_not_this") == 0);
    /* a pid that cannot be alive */
    assert(raopd_pid_publish(pidf, 999999) == 0);
    assert(raopd_pid_is_ours(pidf, "raopd") == 0);
    close(fd); unlink(lock); unlink(pidf);
    return 0;
}
