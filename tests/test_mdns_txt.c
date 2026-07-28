#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "raop_mdns.h"

int main(void) {
    uint8_t out[64];
    size_t entry_off[8];

    /* happy path: 3 entries, correctly length-prefixed and packed contiguously */
    int n = raop_txt_to_wire("txtvers=1\nch=2\ncn=1", out, sizeof out, entry_off, 8);
    assert(n == 3);
    assert(entry_off[0] == 0);
    assert(out[0] == 9);
    assert(memcmp(out + 1, "txtvers=1", 9) == 0);
    assert(entry_off[1] == 10);
    assert(out[10] == 4);
    assert(memcmp(out + 11, "ch=2", 4) == 0);
    assert(entry_off[2] == 15);
    assert(out[15] == 4);
    assert(memcmp(out + 16, "cn=1", 4) == 0);

    /* cap too small (20 bytes needed, 19 given) -> -1, no overflow */
    assert(raop_txt_to_wire("txtvers=1\nch=2\ncn=1", out, 19, entry_off, 8) == -1);

    /* too few entry slots -> -1 */
    assert(raop_txt_to_wire("txtvers=1\nch=2\ncn=1", out, sizeof out, entry_off, 2) == -1);

    /* over-long entry (> 255 bytes) -> -1 */
    char big[300];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    assert(raop_txt_to_wire(big, out, sizeof out, entry_off, 8) == -1);

    return 0;
}
