#ifndef SHAIRKINDLE_WAKELOCK_H
#define SHAIRKINDLE_WAKELOCK_H

/* Keep the K3 awake while audio is playing. Tied to the session lifecycle:
 * wakelock_acquire() at RECORD (a session started), wakelock_release() at
 * TEARDOWN. Idempotent — release-without-acquire and double-acquire are safe.
 *
 * Linux-only (drives lab126 powerd over lipc); a no-op on the host so the
 * --smoke build links and the daemon logic stays testable off-device. */
void wakelock_acquire(void);
void wakelock_release(void);

#endif
