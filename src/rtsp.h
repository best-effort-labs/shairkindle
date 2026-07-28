/* src/rtsp.h — RTSP session state machine (IDLE -> ANNOUNCED -> SETUP ->
 * PLAYING, FLUSH self-loop, TEARDOWN -> IDLE) + response construction.
 *
 * Pure / I-O free: rtsp_handle() only reads an already-parsed rtsp_msg_t and
 * writes a response buffer plus an rtsp_action_t telling the daemon
 * what side effect to perform (bind sockets, start the player, flush at a
 * boundary, tear down). No sockets, no allocation.
 */
#ifndef SHAIRKINDLE_RTSP_H
#define SHAIRKINDLE_RTSP_H
#include <stddef.h>
#include <stdint.h>
#include "rtsp_msg.h"
#include "sdp.h"

typedef enum { RTSP_IDLE, RTSP_ANNOUNCED, RTSP_SETUP, RTSP_PLAYING } rtsp_state_t;
typedef enum { ACT_NONE, ACT_BIND_RTP, ACT_START_PLAY, ACT_FLUSH, ACT_TEARDOWN,
               ACT_SET_VOLUME } rtsp_action_t;

typedef struct {
    rtsp_state_t state;
    raop_sdp_t   sdp;               /* valid once ANNOUNCED               */
    int          have_sdp;
    /* Apple-Challenge inputs supplied by the daemon (fixed per interface): */
    uint8_t      local_ip4[4];
    uint8_t      local_mac[6];
    /* SETUP negotiation: */
    int          sender_control_port, sender_timing_port;   /* parsed     */
    int          server_port, control_port, timing_port;    /* ours; set before reply via rtsp_set_local_ports */
    /* RECORD/FLUSH boundary: */
    uint16_t     rtp_seq;  uint32_t rtp_ts;                 /* from RTP-Info */
    int          pending_volume_pct;                         /* -1 = none  */
    int          audio_latency;                              /* reply const */
    /* negotiated outputs consumed by the daemon: */
    rtsp_action_t action;
} rtsp_session_t;

void rtsp_session_init(rtsp_session_t *s, const uint8_t ip4[4], const uint8_t mac[6],
                       int audio_latency);
void rtsp_set_local_ports(rtsp_session_t *s, int server, int control, int timing);
int  rtsp_handle(rtsp_session_t *s, const rtsp_msg_t *req,
                 char *resp, size_t resp_cap, size_t *resp_len);
#endif
