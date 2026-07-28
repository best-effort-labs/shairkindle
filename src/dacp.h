/* src/dacp.h — outbound DACP (Digital Audio Control Protocol) player control.
 *
 * raopd maps a Kindle button token to a DACP command sent back to the AirPlay-1
 * sender (iOS/macOS Music): play/pause, next, prev. This header covers the pure,
 * host-tested pieces (request formatting, debounce, monotonic clock); the socket
 * sender + control thread live in dacp.c and are validated on-device.
 *
 * DACP success status is 204 No Content. All paths are best-effort: a failure is
 * logged and dropped, never fatal, never touches audio.
 */
#ifndef SHAIRKINDLE_DACP_H
#define SHAIRKINDLE_DACP_H
#include <stddef.h>

typedef enum { DACP_PLAYPAUSE = 0, DACP_NEXTITEM = 1, DACP_PREVITEM = 2 } dacp_cmd_t;

/* Format an HTTP/1.0 DACP request into buf[0..cap). host = dotted IPv4 string,
 * 1<=port<=65535, token = already-sanitized Active-Remote. Returns the request
 * length (<cap), or -1 on any error (bad cmd, NULL/over-long host/token, cap too
 * small). Never writes a partial request on error. */
int dacp_format_request(char *buf, size_t cap, dacp_cmd_t cmd,
                        const char *host, unsigned port, const char *token);

/* Monotonic milliseconds via times() -- NEVER clock_gettime (zig-musl vDSO
 * probe SIGSEGVs on this kernel). Wraps ~248d of uptime; fine for a Kindle. */
long mono_ms(void);

/* Return 1 if `cmd` may fire at now_ms (>= window_ms since its last fire for
 * that command), else 0. Per-command last-fire timestamps kept in a static
 * table -- a single-writer guard for the control thread. */
int dacp_debounce_ok(dacp_cmd_t cmd, long now_ms, long window_ms);

/* Resolve-if-needed (mDNS SRV, cached per session) then issue one DACP command
 * for the current session. iface_ip_be = the RAOP-selected interface. Best-effort:
 * logs a redacted [DACP] trace, never throws, never touches audio. Returns the
 * HTTP status (204 on success) or a negative sentinel. (device path) */
int dacp_send_command(dacp_cmd_t cmd, unsigned iface_ip_be);

/* Control-thread config: the reader binds $prefix/control.sock and dispatches
 * NEXT/PREV/PLAYPAUSE datagrams (from the supervisor relay) to dacp_send_command.
 * *stop drives clean shutdown (polled). */
typedef struct { char prefix[256]; unsigned iface_ip_be; volatile int *stop; } dacp_ctl_cfg_t;
void *dacp_control_thread(void *arg);   /* arg = dacp_ctl_cfg_t* */

#endif
