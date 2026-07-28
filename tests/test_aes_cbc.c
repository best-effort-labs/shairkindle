#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "raop_crypto.h"
/* Fixture generated once with:
 *   printf 'HELLO-RAOP-PKT-1' | openssl enc -aes-128-cbc -nopad \
 *     -K 000102030405060708090a0b0c0d0e0f -iv 0f0e0d0c0b0a09080706050403020100 | xxd -i
 * plaintext "HELLO-RAOP-PKT-1" (16 bytes). Replace CIPHER[] with that output. */
int main(void) {
    const uint8_t key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    const uint8_t iv[16]  = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};
    /* 16-byte ciphertext of "HELLO-RAOP-PKT-1" under key/iv above: */
    uint8_t buf[19] = {0}; /* 16 encrypted + 3 trailing partial */
    extern const uint8_t CIPHER16[16];      /* from fixture (Step 2) */
    memcpy(buf, CIPHER16, 16);
    const uint8_t tail[3] = {0xAA,0xBB,0xCC};
    memcpy(buf + 16, tail, 3);
    raop_aes_cbc_decrypt(key, iv, buf, sizeof buf);
    assert(memcmp(buf, "HELLO-RAOP-PKT-1", 16) == 0);   /* block decrypted */
    assert(memcmp(buf + 16, tail, 3) == 0);             /* trailing partial untouched */
    /* Second call with same iv must reproduce the same result (IV reset each packet): */
    memcpy(buf, CIPHER16, 16);
    raop_aes_cbc_decrypt(key, iv, buf, 16);
    assert(memcmp(buf, "HELLO-RAOP-PKT-1", 16) == 0);
    return 0;
}
