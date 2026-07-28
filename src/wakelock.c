#include "wakelock.h"

#if defined(__linux__)
#include <stdlib.h>

/* Drive lab126's powerd over lipc, the same mechanism kinduino's
 * LinuxPlatformBackend uses (`lipc-set-prop -i <obj> <prop> <val>`). BusyBox on
 * the K3 ships `lipc-set-prop`; system() forks a shell, which reaps its own
 * child (no SIGCHLD handling needed here) and is best-effort — a missing/renamed
 * property must not kill the daemon, so the return is intentionally ignored.
 *
 * Sleep on this device is a state machine: Active -> screenSaver -> suspend.
 * `preventScreenSaver 1` blocks the Active->screenSaver transition, which (since
 * suspend follows the screensaver) keeps the K3 awake while a session is live;
 * it held through a full track on hardware. Set at RECORD, cleared at TEARDOWN.
 *
 * VERIFIED on the K3 (2026-07-20): `deferSuspend <ms>` is NOT a periodic
 * timer-reset — powerd rejects it with lipcPropErrInvalidState (0x100) while
 * state=Active. It's a grace-window responder, only valid once powerd is already
 * heading into suspend (via its pre-suspend lipc event). So keep-awake here is
 * preventScreenSaver, not a deferSuspend poll (an earlier guess, now corrected).
 *
 * NOT yet validated: suspend-on-battery. Over the USB tether the K3 is charging,
 * so its idle timer never runs ("Remaining time in this state: Unknown") and it
 * cannot suspend while we're connected. If preventScreenSaver proves insufficient
 * on battery+WiFi, the fix is event-driven: subscribe to powerd's pre-suspend
 * event and abortSuspend/deferSuspend from the handler.
 */
void wakelock_acquire(void) {
    if (system("lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1")) {
        /* best-effort: no powerd (non-Kindle host) just means no wake-lock */
    }
}

void wakelock_release(void) {
    if (system("lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1")) {
        /* best-effort: no powerd (non-Kindle host) just means no wake-lock */
    }
}

#else /* host (macOS): no powerd — no-op so --smoke links */

void wakelock_acquire(void) {}
void wakelock_release(void) {}

#endif
