/* Reads stdin. Env RAOPD_FAKE_MODE: "fast"=drain immediately (default),
 * "slow"=drain ~1 period every 50ms (forces backpressure),
 * "die-after-N"=read N bytes then exit(0) (forces respawn). Byte count -> stderr on exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void){
    const char* mode = getenv("RAOPD_FAKE_MODE"); if(!mode) mode="fast";
    long die_after = 0; if(!strncmp(mode,"die-after-",10)) die_after=atol(mode+10);
    char buf[4096]; long total=0; int slow = !strcmp(mode,"slow");
    for(;;){
        ssize_t n = read(0, buf, slow ? 512 : sizeof buf);
        if(n<=0) break;
        total+=n;
        if(die_after && total>=die_after){ fprintf(stderr,"consumer died at %ld\n",total); return 0; }
        if(slow) usleep(50000);
    }
    fprintf(stderr,"consumer total=%ld\n",total); return 0;
}
