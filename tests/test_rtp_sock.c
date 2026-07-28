/* tests/test_rtp_sock.c */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/rtp.h"

int main(void) {
    rtp_sockets_t s;
    assert(rtp_open(&s) == 0 && s.server_port > 0);

    int c = socket(AF_INET, SOCK_DGRAM, 0);
    assert(c >= 0);

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)s.server_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    assert(sendto(c, "PING", 4, 0, (struct sockaddr *)&to, sizeof to) == 4);

    uint8_t buf[64];
    struct sockaddr_in from;
    ssize_t n;
    for (int i = 0; i < 100 && (n = rtp_recv(s.audio_fd, buf, sizeof buf, &from)) == 0; i++)
        usleep(1000);
    assert(n == 4 && memcmp(buf, "PING", 4) == 0);
    assert(rtp_recv(s.audio_fd, buf, sizeof buf, &from) == 0);   /* now would-block */

    /* Test rtp_sendto: create fresh receiver socket, send from audio_fd to it, verify arrival + port */
    int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(recv_sock >= 0);

    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof recv_addr);
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = htons(0);  /* ephemeral */
    assert(bind(recv_sock, (struct sockaddr *)&recv_addr, sizeof recv_addr) == 0);

    socklen_t addrlen = sizeof recv_addr;
    assert(getsockname(recv_sock, (struct sockaddr *)&recv_addr, &addrlen) == 0);
    uint16_t recv_port = ntohs(recv_addr.sin_port);

    /* Set recv_sock to non-blocking for polling */
    assert(fcntl(recv_sock, F_SETFL, O_NONBLOCK) == 0);

    /* Send "TEST" from audio_fd to recv_sock's port */
    assert(rtp_sendto(s.audio_fd, (const uint8_t *)"TEST", 4, htonl(INADDR_LOOPBACK), recv_port) == 4);

    /* Receive on recv_sock with poll loop, verify bytes and sender port */
    struct sockaddr_in sender;
    for (int i = 0; i < 100 && (n = recvfrom(recv_sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&sender, &addrlen)) <= 0; i++)
        usleep(1000);
    assert(n == 4 && memcmp(buf, "TEST", 4) == 0);
    assert(ntohs(sender.sin_port) == s.server_port);  /* sender is audio_fd's port */

    close(recv_sock);

    close(c);
    rtp_close(&s);
    printf("test_rtp_sock OK\n");
    return 0;
}
