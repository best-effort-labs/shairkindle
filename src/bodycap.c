#include "bodycap.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

void bodycap_begin(bodycap_t *b, bodycap_mode_t mode, long declared_len,
                   const char *final_path, uint8_t *mem, size_t mem_cap){
    memset(b, 0, sizeof *b);
    b->mode = mode;
    b->remaining = declared_len < 0 ? 0 : declared_len;
    b->fd = -1;
    b->mem = mem; b->mem_cap = mem_cap;
    if (mode == BODYCAP_FILE && final_path){
        snprintf(b->final, sizeof b->final, "%s", final_path);
        snprintf(b->tmp, sizeof b->tmp, "%s.tmp", final_path);
        b->fd = open(b->tmp, O_CREAT|O_TRUNC|O_WRONLY, 0600);
        if (b->fd < 0) b->failed = 1;
    }
}

size_t bodycap_feed(bodycap_t *b, const uint8_t *data, size_t len){
    size_t take = (size_t)b->remaining < len ? (size_t)b->remaining : len;
    if (take && !b->failed){
        if (b->mode == BODYCAP_FILE){
            size_t off = 0;
            while (off < take){
                ssize_t w = write(b->fd, data + off, take - off);
                if (w <= 0){ b->failed = 1; break; }
                off += (size_t)w;
            }
        } else if (b->mode == BODYCAP_MEM){
            if (b->mem_len + take > b->mem_cap){
                size_t fit = b->mem_cap - b->mem_len;
                if (fit) memcpy(b->mem + b->mem_len, data, fit);
                b->mem_len = b->mem_cap;
                b->failed = 1;                  /* over-cap: drop excess, mark failed */
            } else {
                memcpy(b->mem + b->mem_len, data, take);
                b->mem_len += take;
            }
        }
        /* DISCARD: nothing to store */
    }
    b->remaining -= (long)take;                 /* always advance so RTSP stays framed */
    return take;
}

int bodycap_done(const bodycap_t *b){ return b->remaining == 0; }

int bodycap_finish(bodycap_t *b){
    if (b->mode == BODYCAP_FILE){
        int ok = bodycap_done(b) && !b->failed;
        if (b->fd >= 0){
            if (ok) fsync(b->fd);
            close(b->fd); b->fd = -1;
        }
        if (ok && rename(b->tmp, b->final) == 0) return 0;
        unlink(b->tmp);
        return -1;
    }
    return (bodycap_done(b) && !b->failed) ? 0 : -1;
}
