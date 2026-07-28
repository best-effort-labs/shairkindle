/* tests/rtsp_probe.c — tiny RTSP loopback probe for test_rtsp_loopback.sh.
 * Connects to 127.0.0.1:<port>, drives two exchanges against a running
 * `raopd --smoke <port>`, and asserts the responses. No `nc` dependency.
 *
 *   probe 1: OPTIONS      -> response has "RTSP/1.0 200" and "CSeq: 1"
 *   probe 2: ANNOUNCE+SETUP -> SETUP 200 carries a NONZERO server_port=
 *            (proves main bound RTP + rtsp_set_local_ports ran before rtsp_handle)
 *
 * The ANNOUNCE reuses the Task-1 SDP fixture (valid rsaaeskey/aesiv/fmtp) so
 * sdp_parse accepts it and the session advances IDLE->ANNOUNCED->SETUP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "fixture_sdp.h"   /* SDP_TEMPLATE */

/* Same known-good base64 constants as tests/test_rtsp.c / test_sdp.c. */
static const char RSAKEY_B64[] =
  "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4"
  "OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fYGFiY2RlZmdoaWprbG1ub3Bx"
  "cnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6PkJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmq"
  "q6ytrq+wsbKztLW2t7i5uru8vb6/wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj"
  "5OXm5+jp6uvs7e7v8PHy8/T19vf4+fr7/P3+/w==";
static const char AESIV_B64[] = "ESIzRFVmd4iZqrvM3e7/AA==";

static int dial(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

/* Send `req`, read one response into buf (until the socket goes quiet or full). */
static int exchange(int fd, const char *req, size_t reqlen, char *buf, size_t cap) {
    if (write(fd, req, reqlen) != (ssize_t)reqlen) return -1;
    /* One RTSP response fits in a single small read for these probes. */
    ssize_t n = read(fd, buf, cap - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int)n;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: rtsp_probe <port>\n"); return 2; }
    int port = atoi(argv[1]);
    char resp[4096];

    /* Probe 1: OPTIONS on a fresh connection. */
    int fd = dial(port);
    if (fd < 0) { fprintf(stderr, "connect failed\n"); return 1; }
    const char *opt = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    if (exchange(fd, opt, strlen(opt), resp, sizeof resp) < 0) {
        fprintf(stderr, "OPTIONS exchange failed\n"); close(fd); return 1;
    }
    if (!strstr(resp, "RTSP/1.0 200") || !strstr(resp, "CSeq: 1")) {
        fprintf(stderr, "OPTIONS bad response:\n%s\n", resp); close(fd); return 1;
    }

    /* Probe 2: ANNOUNCE then SETUP on the same connection. */
    char sdp[1024], ann[2048];
    int slen = snprintf(sdp, sizeof sdp, SDP_TEMPLATE, RSAKEY_B64, AESIV_B64);
    int an = snprintf(ann, sizeof ann,
        "ANNOUNCE rtsp://x/1 RTSP/1.0\r\nCSeq: 2\r\n"
        "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s", slen, sdp);
    if (exchange(fd, ann, (size_t)an, resp, sizeof resp) < 0 ||
        !strstr(resp, "RTSP/1.0 200")) {
        fprintf(stderr, "ANNOUNCE bad response:\n%s\n", resp); close(fd); return 1;
    }

    const char *setup =
        "SETUP rtsp://x/1 RTSP/1.0\r\nCSeq: 3\r\n"
        "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=55001;timing_port=55002\r\n\r\n";
    if (exchange(fd, setup, strlen(setup), resp, sizeof resp) < 0 ||
        !strstr(resp, "RTSP/1.0 200")) {
        fprintf(stderr, "SETUP bad response:\n%s\n", resp); close(fd); return 1;
    }
    /* The port-ordering assertion: server_port must be present and NONZERO. */
    const char *sp = strstr(resp, "server_port=");
    if (!sp) { fprintf(stderr, "no server_port in SETUP:\n%s\n", resp); close(fd); return 1; }
    int server_port = atoi(sp + strlen("server_port="));
    if (server_port <= 0) {
        fprintf(stderr, "server_port not bound (=%d) — port-ordering broken:\n%s\n",
                server_port, resp);
        close(fd); return 1;
    }

    close(fd);
    printf("rtsp_probe OK (server_port=%d)\n", server_port);
    return 0;
}
