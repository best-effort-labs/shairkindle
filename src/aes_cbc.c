#include "raop_crypto.h"
#include <string.h>
#include "bearssl_block.h"
void raop_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                          uint8_t *buf, size_t len) {
    size_t whole = len & ~(size_t)15;           /* largest 16-multiple */
    if (whole == 0) return;                       /* nothing to do */
    br_aes_big_cbcdec_keys ctx;
    br_aes_big_cbcdec_init(&ctx, key, 16);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);                       /* run() mutates iv -> use a copy */
    br_aes_big_cbcdec_run(&ctx, iv_copy, buf, whole);
}
