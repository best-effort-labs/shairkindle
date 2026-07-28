#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "raop_crypto.h"
/* Fixture: a 16-byte AES key OAEP-encrypted to the AirPort PUBLIC key. See
 * tests/fixture_oaep.c (openssl pkeyutl -encrypt ... rsa_padding_mode:oaep
 * rsa_oaep_md:sha1 over 16 * 'K'). */
int main(void) {
    extern const uint8_t CT[]; extern const unsigned CT_LEN;
    uint8_t key[16];
    int ok = raop_oaep_unwrap_key(CT, CT_LEN, key);
    assert(ok == 1);
    for (int i = 0; i < 16; i++) assert(key[i] == 'K');
    /* corrupt ciphertext -> must fail, not crash */
    uint8_t bad[512]; assert(CT_LEN <= sizeof bad);
    memcpy(bad, CT, CT_LEN); bad[0] ^= 0xFF;
    assert(raop_oaep_unwrap_key(bad, CT_LEN, key) == 0);
    return 0;
}
