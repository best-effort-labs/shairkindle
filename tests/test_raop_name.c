#include "raop_name.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static const char *san(const char *in){
    static char buf[64];
    raop_name_sanitize(in, buf, sizeof buf);
    return buf;
}

int main(void){
    /* empty / NULL -> default */
    assert(strcmp(san(""),   "ShairKindle") == 0);
    assert(strcmp(san(NULL), "ShairKindle") == 0);

    /* plain passthrough (spaces & ordinary punctuation are valid) */
    assert(strcmp(san("Living Room"), "Living Room") == 0);
    assert(strcmp(san("Tom's Kindle"), "Tom's Kindle") == 0);

    /* control chars + DEL stripped */
    assert(strcmp(san("Ab\x01\x1f\x7f" "cd"), "Abcd") == 0);

    /* dots dropped (they restructure the DNS name in tinysvcmdns) */
    assert(strcmp(san("192.168.1.1"), "19216811") == 0);

    /* stray '@' dropped (Apple clients may split on a second '@') */
    assert(strcmp(san("a@b"), "ab") == 0);

    /* whitespace collapsed + trimmed */
    assert(strcmp(san("  a   b  "), "a b") == 0);

    /* 50-byte cap on ASCII */
    char sixty[61]; memset(sixty, 'x', 60); sixty[60] = 0;
    assert(strlen(san(sixty)) == 50);

    /* UTF-8 truncation on a code-point boundary: 30x 'é' (0xC3 0xA9) = 60 bytes.
       Cap 50 must land on an even byte (25 chars = 50 bytes), never a lone 0xC3. */
    char utf[61]; for (int i = 0; i < 30; i++){ utf[2*i]=(char)0xC3; utf[2*i+1]=(char)0xA9; } utf[60]=0;
    const char *u = san(utf);
    size_t ulen = strlen(u);
    assert(ulen <= 50);
    assert((unsigned char)u[ulen-1] == 0xA9);   /* ends on a complete code point */
    assert(ulen % 2 == 0);

    /* full instance MAC@name stays <= 63 bytes */
    char inst[96];
    snprintf(inst, sizeof inst, "AABBCCDDEEFF@%s", san(sixty));
    assert(strlen(inst) <= 63);

    printf("test_raop_name OK\n");
    return 0;
}
