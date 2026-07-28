#ifndef RAOP_CRYPTO_H
#define RAOP_CRYPTO_H
#include <stdint.h>
#include <stddef.h>
/* Decrypt the largest 16*k prefix of buf in place; trailing bytes untouched.
   IV is reset from the caller's iv each call (RAOP resets per packet). */
void raop_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                          uint8_t *buf, size_t len);
/* Unwrap the 16-byte AES session key from the SDP rsaaeskey via RSAES-OAEP
   (SHA-1) with the AirPort private key. Returns 1 + fills out_key iff it
   recovers exactly 16 bytes, else 0. */
int raop_oaep_unwrap_key(const uint8_t *rsaaeskey, size_t n, uint8_t out_key[16]);
/* Sign an RTSP Apple-Challenge: build challenge‖ip4‖mac zero-padded to 32 bytes,
   PKCS#1 type-1 pad to the modulus width, raw RSA private op with the AirPort key,
   base64 (no '=' padding) into out_b64 (NUL-terminated). Returns b64 length or -1. */
int raop_apple_response(const uint8_t *challenge, size_t clen,
                        const uint8_t ip4[4], const uint8_t mac[6],
                        char *out_b64, size_t out_cap);
#endif
