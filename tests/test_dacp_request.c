#include "dacp.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

int main(void) {
    char buf[512];
    int n = dacp_format_request(buf, sizeof buf, DACP_PLAYPAUSE, "192.168.1.20", 49372, "123456");
    const char *want =
        "GET /ctrl-int/1/playpause HTTP/1.0\r\n"
        "Host: 192.168.1.20:49372\r\n"
        "Active-Remote: 123456\r\n"
        "Connection: close\r\n\r\n";
    assert(n == (int)strlen(want));
    assert(memcmp(buf, want, (size_t)n) == 0);

    /* command path mapping */
    assert(dacp_format_request(buf, sizeof buf, DACP_NEXTITEM, "10.0.0.1", 1, "9") > 0);
    assert(strstr(buf, "GET /ctrl-int/1/nextitem HTTP/1.0\r\n") == buf);
    assert(dacp_format_request(buf, sizeof buf, DACP_PREVITEM, "10.0.0.1", 65535, "9") > 0);
    assert(strstr(buf, "GET /ctrl-int/1/previtem HTTP/1.0\r\n") == buf);

    /* rejects: bad port, over-small buffer, over-long host, bad cmd */
    assert(dacp_format_request(buf, sizeof buf, DACP_PLAYPAUSE, "10.0.0.1", 0, "9") == -1);
    assert(dacp_format_request(buf, sizeof buf, DACP_PLAYPAUSE, "10.0.0.1", 70000, "9") == -1);
    assert(dacp_format_request(buf, 8, DACP_PLAYPAUSE, "10.0.0.1", 1, "9") == -1);
    assert(dacp_format_request(buf, sizeof buf, (dacp_cmd_t)99, "10.0.0.1", 1, "9") == -1);
    char longhost[64]; memset(longhost, '9', sizeof longhost - 1); longhost[63] = 0;
    assert(dacp_format_request(buf, sizeof buf, DACP_PLAYPAUSE, longhost, 1, "9") == -1);
    printf("test_dacp_request: OK\n");
    return 0;
}
