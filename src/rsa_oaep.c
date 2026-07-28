#include "raop_crypto.h"
#include <string.h>
#include "bearssl_rsa.h"
#include "bearssl_hash.h"
#include "airport_key.h"
int raop_oaep_unwrap_key(const uint8_t *rsaaeskey, size_t n, uint8_t out_key[16]) {
    size_t klen = (AIRPORT_RSA_SK.n_bitlen + 7) / 8;
    if (n != klen) return 0;
    uint8_t buf[512];
    if (klen > sizeof buf) return 0;
    memcpy(buf, rsaaeskey, klen);
    size_t len = klen;
    /* empty label, SHA-1 hash + MGF1-SHA1 */
    uint32_t rc = br_rsa_i31_oaep_decrypt(&br_sha1_vtable, NULL, 0,
                                          &AIRPORT_RSA_SK, buf, &len);
    if (rc != 1 || len != 16) return 0;
    memcpy(out_key, buf, 16);
    return 1;
}
