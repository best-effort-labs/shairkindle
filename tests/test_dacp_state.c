#include "dacp_state.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

int main(void) {
    /* --- validators --- */
    char id[17];
    assert(dacp_normalize_id(id, "56b2a4d4", 8) == 0 && strcmp(id, "56B2A4D4") == 0);
    assert(dacp_normalize_id(id, "0011223344556677", 16) == 0 && strcmp(id, "0011223344556677") == 0);
    assert(dacp_normalize_id(id, "", 0) == -1);              /* empty */
    assert(dacp_normalize_id(id, "xyz", 3) == -1);           /* non-hex */
    assert(dacp_normalize_id(id, "00112233445566778", 17) == -1);  /* >16 */

    char tok[32];
    assert(dacp_sanitize_token(tok, sizeof tok, "123456", 6) == 0 && strcmp(tok, "123456") == 0);
    assert(dacp_sanitize_token(tok, sizeof tok, "123\r\nEvil: x", 11) == 0 && strcmp(tok, "123") == 0);
    assert(dacp_sanitize_token(tok, sizeof tok, "abc", 3) == -1);   /* no leading digits */
    assert(dacp_sanitize_token(tok, sizeof tok, "", 0) == -1);      /* empty */
    char big[40]; memset(big, '9', sizeof big);
    assert(dacp_sanitize_token(tok, sizeof tok, big, sizeof big) == -1);  /* overflows 32-cap */
    printf("test_dacp_state(validators): OK\n");

    /* --- state API --- */
    dacp_state_init();
    dacp_snapshot_t s0; dacp_state_snapshot(&s0);
    assert(!s0.have_creds && s0.resolved_port == 0);
    dacp_state_capture("56b2a4d4", 8, "123456", 6, 0x1401A8C0 /* 192.168.1.20 BE */);
    dacp_snapshot_t s1; dacp_state_snapshot(&s1);
    assert(s1.have_creds && strcmp(s1.dacp_id, "56B2A4D4") == 0 && strcmp(s1.active_remote, "123456") == 0);
    assert(s1.peer_ip_be == 0x1401A8C0);
    unsigned g1 = s1.generation;
    dacp_state_publish_port(g1, 49372);
    dacp_snapshot_t s2; dacp_state_snapshot(&s2);
    assert(s2.resolved_port == 49372 && s2.resolved_gen == g1);
    dacp_state_new_session();                 /* generation++ clears everything */
    dacp_snapshot_t s3; dacp_state_snapshot(&s3);
    assert(!s3.have_creds && s3.resolved_port == 0 && s3.generation == g1 + 1);
    dacp_state_publish_port(g1, 55555);       /* stale gen -> dropped */
    dacp_state_snapshot(&s3);
    assert(s3.resolved_port == 0);
    /* invalid creds don't publish */
    dacp_state_capture("zz", 2, "123", 3, 0x1401A8C0);
    dacp_state_snapshot(&s3); assert(!s3.have_creds);
    printf("test_dacp_state(full): OK\n");

    /* identity change WITHIN a session (no new_session) clears the cached port */
    dacp_state_init();
    dacp_state_capture("aabb", 4, "111", 3, 0x01020304);
    dacp_snapshot_t i1; dacp_state_snapshot(&i1);
    dacp_state_publish_port(i1.generation, 40000);
    dacp_state_snapshot(&i1); assert(i1.resolved_port == 40000);
    dacp_state_capture("ccdd", 4, "222", 3, 0x01020304);          /* different id+token */
    dacp_snapshot_t i2; dacp_state_snapshot(&i2);
    assert(i2.resolved_port == 0 && strcmp(i2.dacp_id, "CCDD") == 0 && i2.generation == i1.generation);
    /* identical creds re-sent does NOT clear a freshly published port */
    dacp_state_publish_port(i2.generation, 40001);
    dacp_state_capture("ccdd", 4, "222", 3, 0x01020304);
    dacp_state_snapshot(&i2); assert(i2.resolved_port == 40001);
    printf("test_dacp_state(identity-change): OK\n");
    return 0;
}
