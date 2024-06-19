#ifndef __EZCACHE_H__
#define __EZCACHE_H__

#include "HM2P_Common.h"

enum {
    SYS_CACHE_VIDEO_MAIN = 0,
    SYS_CACHE_VIDEO_SUB,
    SYS_CACHE_AUDIO,
    SYS_CACHE_COUNT
};


typedef struct ezcache_frm_s
{
    queue_t             queue;
    long long  seq;
    unsigned long long  ts;

    char                typ;    // 0:audio  1:iframe  2:pframe
    int                 datan;
    char                data[0];
} ezcache_frm_t;


int ezcache_exit(int channel_id);
int ezcache_frm_add(int channel_id, char * data, int datan, int typ, unsigned long long ts);
ezcache_frm_t * ezcache_frm_get(int channel_id, long long seq);
long long ezcache_last_idr(int channel_id);
long long ezcache_prev_idr(int channel_id, long long seq);

#endif

