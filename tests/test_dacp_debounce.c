#include "dacp.h"
#include <stdio.h>
#include <assert.h>

int main(void) {
    assert(dacp_debounce_ok(DACP_NEXTITEM, 1000, 300) == 1);   /* first fire ok */
    assert(dacp_debounce_ok(DACP_NEXTITEM, 1200, 300) == 0);   /* within window */
    assert(dacp_debounce_ok(DACP_NEXTITEM, 1400, 300) == 1);   /* window elapsed */
    assert(dacp_debounce_ok(DACP_PREVITEM, 1450, 300) == 1);   /* different cmd, independent */
    assert(dacp_debounce_ok(DACP_PLAYPAUSE, 1450, 300) == 1);  /* independent again */
    assert(dacp_debounce_ok(DACP_PLAYPAUSE, 1600, 300) == 0);  /* within window */
    /* mono_ms is monotonic non-negative */
    long a = mono_ms(); long b = mono_ms();
    assert(a >= 0 && b >= a);
    printf("test_dacp_debounce: OK\n");
    return 0;
}
