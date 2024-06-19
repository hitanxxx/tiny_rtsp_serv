#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/vfs.h>
#include <assert.h>


#include "hardware_platform.h"
#include "ezcache.h"


#define CACHE_SIZE_MAX  (2*1024*1024)    ///max cache size of each channel

static queue_t g_cache_frm_queue[2];
static int g_cache_frm_init[2] = {0};
static int g_cache_frm_cnt[2] = {0};
static int g_cache_frm_totaln[2] = {0};
static long long g_cache_frm_seq[2] = {0};
static pthread_mutex_t g_cache_frm_lock = PTHREAD_MUTEX_INITIALIZER;


int ezcache_init(int channel_id)
{
    if((channel_id < 0) || (channel_id > 1)) {
        err("channel_id [%d] not support\n", channel_id);
        return -1;
    }
    if(g_cache_frm_init[channel_id]) {
        err("channel_id [%d] cc already init\n");
        return -1;
    }
    queue_init(&g_cache_frm_queue[channel_id]);
    return 0;
}

int ezcache_exit(int channel_id)
{
    if((channel_id < 0) || (channel_id > 1)) {
        err("channel_id [%d] not support\n", channel_id);
        return -1;
    }
    if(!g_cache_frm_init[channel_id]) {
        err("channle_id [%d] cc not init\n", channel_id);
        return -1;
    }
    ///todo: run loop and clear each channel
    return 0;
}

int ezcache_frm_add(int channel_id, char * data, int datan, int typ, unsigned long long ts)
{
    if((channel_id < 0) || (channel_id > 1)) {
        err("channel_id [%d] not support\n", channel_id);
        return -1;
    }
    if((typ < 0) || (typ > 2)) {
        err("frm typ [%d] not support\n", typ);
        return -1;
    }

    if(!g_cache_frm_init[channel_id]) { ///only initialization cache when idr frame comes if cache not initialization
        if(typ != 1) {
            return 0;
        } else {
            ezcache_init(channel_id);
            g_cache_frm_init[channel_id] = 1;
        }
    }
    pthread_mutex_lock(&g_cache_frm_lock);
    while(g_cache_frm_totaln[channel_id] >= CACHE_SIZE_MAX) { ///delete the oldest frame. [frist in the queue]
        queue_t * q = queue_head(&g_cache_frm_queue[channel_id]);
        ezcache_frm_t * oldest_frm = ptr_get_struct(q, ezcache_frm_t, queue);
        queue_remove(q);
        g_cache_frm_totaln[channel_id] -= oldest_frm->datan;
        g_cache_frm_cnt[channel_id] --;
        sys_free(oldest_frm);
    }

    ezcache_frm_t * new_frm = sys_alloc(sizeof(ezcache_frm_t)+datan);
    if(!new_frm) {
        err("alloc frm len [%d] failed. [%d]\n", sizeof(ezcache_frm_t)+datan, errno);
        pthread_mutex_unlock(&g_cache_frm_lock);
        return -1;
    }
    queue_insert_tail(&g_cache_frm_queue[channel_id], &new_frm->queue);
    memcpy(new_frm->data, data, datan);
    new_frm->datan = datan;
    new_frm->seq = g_cache_frm_seq[channel_id]++;
    new_frm->ts = ts;
    new_frm->typ = typ;
    //dbg("channel [%d] add frm len [%d]. frm cnt [%d] totaln [%d]\n", channel_id, datan, g_cache_frm_cnt[channel_id], g_cache_frm_totaln[channel_id]);
    g_cache_frm_totaln[channel_id] += datan;
    g_cache_frm_cnt[channel_id] ++;
    pthread_mutex_unlock(&g_cache_frm_lock);
    return 0;   
}

ezcache_frm_t * ezcache_frm_get(int channel_id, long long seq)
{
    ezcache_frm_t * frm = NULL;
    int found = 0;

    if((channel_id < 0) || (channel_id > 1)) {
        err("channel_id [%d] not support\n", channel_id);
        return NULL;
    }
    if(!g_cache_frm_init[channel_id]) 
        return NULL;
    
    if((seq < 0) || (seq >= g_cache_frm_seq[channel_id])) {
        //err("seq [%lld] out of range. current seq max [%lld]\n", seq, g_cache_frm_seq[channel_id]);
        return NULL;
    }
        
    pthread_mutex_lock(&g_cache_frm_lock);
    if((seq * 2) <= g_cache_frm_seq[channel_id]) {  /// find start with head
        queue_t * q = queue_head(&g_cache_frm_queue[channel_id]);
        for(; q != queue_tail(&g_cache_frm_queue[channel_id]); q = queue_next(q)) {
            frm = ptr_get_struct(q, ezcache_frm_t, queue);
            if(frm->seq == seq) {
                found = 1;
                break;
            }
        }
    } else { /// find start with tail
        queue_t * q = queue_prev(&g_cache_frm_queue[channel_id]);
        for(; q != queue_tail(&g_cache_frm_queue[channel_id]); q = queue_prev(q)) {
            frm = ptr_get_struct(q, ezcache_frm_t, queue);
            if(frm->seq == seq) {
                found = 1;
                break;
            }
        }
    }
    if(found) {
        ezcache_frm_t * new_frm = sys_alloc(sizeof(ezcache_frm_t)+frm->datan);
        if(new_frm) {
            memcpy(new_frm, frm, sizeof(ezcache_frm_t)+frm->datan);
            pthread_mutex_unlock(&g_cache_frm_lock);
            return new_frm;
        } else {
            err("alloc new frm failed. [%d] [%s]\n", errno, strerror(errno));
            pthread_mutex_unlock(&g_cache_frm_lock);
            return NULL;
        }
    }
    pthread_mutex_unlock(&g_cache_frm_lock);
    return NULL;
}

long long ezcache_prev_idr(int channel_id, long long seq)
{
    if((channel_id < 0) || (channel_id > 1)) {
        err("channel_id [%d] not support\n", channel_id);
        return -1;
    }
    if(!g_cache_frm_init[channel_id]) 
        return -1;
    
    if((seq < 0) || (seq >= g_cache_frm_seq[channel_id])) {
        err("seq [%lld] out of range. current seq max [%lld]\n", seq, g_cache_frm_seq[channel_id]);
        return -1;
    }

    queue_t * q = queue_head(&g_cache_frm_queue[channel_id]);
    for(; q != queue_tail(&g_cache_frm_queue[channel_id]); q = queue_next(q)) {
        ezcache_frm_t * frm = ptr_get_struct(q, ezcache_frm_t, queue);
        if(frm->seq == seq) {
            queue_t * t = queue_prev(q); ///forward traserval find previously idr frame
            for(; t != queue_tail(&g_cache_frm_queue[channel_id]); t = queue_prev(t)) {
                ezcache_frm_t * frmm = ptr_get_struct(t, ezcache_frm_t, queue);
                if(frmm->typ == 1) {
                    return frmm->seq;
                }
            }
        }
    }
    err("sys precv idr by specify not found\n");
    return -1;
}

long long ezcache_last_idr(int channel_id)
{
    /// forward traserval, find the last dir frame seq number 
    if((channel_id < 0) || (channel_id > 1)) {
        err("channel_id [%d] not support\n", channel_id );
        return -1;
    }
    if(!g_cache_frm_init[channel_id])
        return -1;
    
    queue_t * q = queue_prev(&g_cache_frm_queue[channel_id]);
    for(; q != queue_tail(&g_cache_frm_queue[channel_id]); q = queue_prev(q)) {
        ezcache_frm_t * frm = ptr_get_struct(q, ezcache_frm_t, queue);
        if(frm->typ == 1) {
            return frm->seq;
        }
    }
    return -1;
}


