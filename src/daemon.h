#ifndef RAOPD_DAEMON_H
#define RAOPD_DAEMON_H
#include <sys/types.h>
int raopd_lock_acquire(const char *lockpath);
int raopd_pid_publish(const char *pidpath, pid_t pid);
int raopd_pid_is_ours(const char *pidpath, const char *exe_basename);
#endif
