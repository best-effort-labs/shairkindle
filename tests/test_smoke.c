#include <assert.h>
#include <string.h>
#include "version.h"

int main(void) {
    assert(strlen(RAOPD_VERSION) > 0);
    return 0;
}
