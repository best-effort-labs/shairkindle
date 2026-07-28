#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "raop_crypto.h"
/* A correct response is verifiable: RSA-public(response) == the type-1-padded
 * payload. This test asserts round-trippable structure via a known-answer blob
 * generated once with a reference (hairtunes/shairport) for the same inputs:
 *   challenge = 16 bytes 0x01..0x10, ip4 = 10.0.0.2, mac = 00:11:22:33:44:55
 * Paste the reference base64 (minus '=') into EXPECT[]. */
int main(void) {
    uint8_t ch[16]; for (int i=0;i<16;i++) ch[i]=(uint8_t)(i+1);
    const uint8_t ip4[4] = {10,0,0,2};
    const uint8_t mac[6] = {0x00,0x11,0x22,0x33,0x44,0x55};
    char out[1024];
    int n = raop_apple_response(ch, 16, ip4, mac, out, sizeof out);
    assert(n > 0);
    assert(strchr(out, '=') == NULL);        /* padding stripped */
    extern const char EXPECT[];
    assert(strcmp(out, EXPECT) == 0);
    return 0;
}
