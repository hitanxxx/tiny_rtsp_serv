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

#if defined(FAC)
#include "HM2P_FactoryTest.h"
#include "audioTest/audioTest.h"
#else
// sdk header file
#include "HM2P_P2P.h"
#endif
#include "hardware_platform.h"
#include "HM2P_Common.h"
#include "sys_cache.h"


#define CACHE_SIZE_MAX      (2*1024*1024)

static queue_t g_cache_frm_queue[2];
static int g_cache_frm_init[2] = {0};
static int g_cache_frm_cnt[2] = {0};
static int g_cache_frm_totaln[2] = {0};
static long long g_cache_frm_seq[2] = {0};

static pthread_mutex_t g_cache_frm_lock = PTHREAD_MUTEX_INITIALIZER;



/// @brief init a sys frame cache channel
/// @param channel_id [IN] channel id [0,1]
/// @return 
int sys_cache_init( int channel_id )
{
    if( channel_id < 0 || channel_id > 1 ) {
        err("channel_id [%d] not support yet\n", channel_id );
        return -1;
    }
    queue_init( &g_cache_frm_queue[channel_id] );
    return 0;
}


/// @brief exit a sys frame cache channel 
/// @param channel_id [IN] channel id [0,1]
/// @return 
int sys_cache_exit( int channel_id )
{
    if( channel_id < 0 || channel_id > 1 ) {
        err("channel_id [%d] not support yet\n", channel_id );
        return -1;
    }

    /// run loop and clear 
    return 0;
}


/// @brief add a frame into channel cache 
/// @param channel_id [IN] 0:main channel 1:sub channel
/// @param data [IN] frame data
/// @param datan [IN] frame datan
/// @param typ [IN] 0:audio frame 1:iframe 2:pframe
/// @param ts [IN] system time tick
/// @return 0:success -1:failed
int sys_cache_frm_add( int channel_id, char * data, int datan, int typ, unsigned long long ts )
{
    if( channel_id < 0 || channel_id > 1 ) {
        err("channel_id [%d] not support yet\n", channel_id );
        return -1;
    }
    if( typ < 0 || typ > 2 ) {
        err("frm typ [%d] not support yet\n", typ);
        return -1;
    }

    /// if not init
    if( g_cache_frm_init[channel_id] != 1 ) {
        if( typ == 0 ) {
            /// audio frame. do nothing 
            return 0;
        } else if ( typ == 1 || typ == 2 ) {
            /// video frame. init frm queue
            sys_cache_init(channel_id);
            g_cache_frm_init[channel_id] = 1;
        }
    }

    /// lock make sure Thread safety
    pthread_mutex_lock(&g_cache_frm_lock);

    while( g_cache_frm_totaln[channel_id] >= CACHE_SIZE_MAX ) {        
        /// find the oldest frame. [frist in the queue]
        queue_t * q = queue_head( &g_cache_frm_queue[channel_id] );
        sys_cache_frm_t * oldest_frm = ptr_get_struct( q, sys_cache_frm_t, queue );
        queue_remove( q );

        g_cache_frm_totaln[channel_id] -= oldest_frm->datan;
        g_cache_frm_cnt[channel_id] --;

        /// free oldest frame 
        sys_free( oldest_frm );
    }

    /// alloc new frame
    sys_cache_frm_t * new_frm = sys_alloc( sizeof(sys_cache_frm_t) + datan );
    if( !new_frm ) {
        err("alloc frm len [%d] failed. [%d]\n", sizeof(sys_cache_frm_t) + datan, errno );
        pthread_mutex_unlock(&g_cache_frm_lock);
        return -1;
    }
    
    /// add new frame into channel cache
    queue_insert_tail( &g_cache_frm_queue[channel_id], &new_frm->queue );
    memcpy( new_frm->data, data, datan );
    new_frm->datan = datan;
    new_frm->seq = g_cache_frm_seq[channel_id]++;
    new_frm->ts = ts;
    new_frm->typ = typ;

    //dbg("channel [%d] add frm len [%d]. frm cnt [%d] totaln [%d]\n", channel_id, datan, g_cache_frm_cnt[channel_id], g_cache_frm_totaln[channel_id] );

    /// change total value
    g_cache_frm_totaln[channel_id] += datan;
    g_cache_frm_cnt[channel_id] ++;
    pthread_mutex_unlock(&g_cache_frm_lock);
    return 0;   
}


/// @brief find the frame of specify seq number
/// @param channel_id [IN] channel id 
/// @param seq [IN] specify seq number
/// @return success:frame addr failed:NULL
sys_cache_frm_t * sys_cache_frm_get( int channel_id, long long seq )
{
    if( channel_id < 0 || channel_id > 1 ) {
        err("channel_id [%d] not support yet\n", channel_id );
        return NULL;
    }
    if( seq < 0 || seq >= g_cache_frm_seq[channel_id] ) {
        //err("seq [%lld] out of range. current seq max [%lld]\n", seq, g_cache_frm_seq[channel_id] );
        return NULL;
    }
    
    int found = 0;
    sys_cache_frm_t * frm = NULL;
    
    pthread_mutex_lock(&g_cache_frm_lock);

    if( seq * 2 <= g_cache_frm_seq[channel_id] ) {
        /// find start with head
        queue_t * q = queue_head( &g_cache_frm_queue[channel_id] );
        for( ; q != queue_tail( &g_cache_frm_queue[channel_id] ); q = queue_next(q) ) {
            frm = ptr_get_struct( q, sys_cache_frm_t, queue );
            if( frm->seq == seq ) {
                found = 1;
                break;
            }
        }
    } else {
        /// find start with tail
        queue_t * q = queue_prev( &g_cache_frm_queue[channel_id] );
        for( ; q != queue_tail(&g_cache_frm_queue[channel_id]); q = queue_prev( q ) ) {
            frm = ptr_get_struct( q, sys_cache_frm_t, queue );
            if( frm->seq == seq ) {
                found = 1;
                break;
            }
        }
    }

    if( found == 1 ) {
        sys_cache_frm_t * new_frm = sys_alloc( sizeof(sys_cache_frm_t) + frm->datan );
        if( new_frm ) {
            memcpy( new_frm, frm,  sizeof(sys_cache_frm_t) + frm->datan );
            pthread_mutex_unlock(&g_cache_frm_lock);
            return new_frm;
        } else {
            err("alloc new frm failed. [%d] [%s]\n", errno, strerror(errno) );
            pthread_mutex_unlock(&g_cache_frm_lock);
            return NULL;
        }
    }

    pthread_mutex_unlock(&g_cache_frm_lock);
    return NULL;
}



/// @brief find the previously iframe of specify iframe seq number
/// @param channel_id [IN] channel id 
/// @param seq [IN] specify seq number of iframe 
/// @return -1:not found  >=0: seq number
long long sys_cache_prev_idr( int channel_id, long long seq )
{
    if( channel_id < 0 || channel_id > 1 ) {
        err("channel_id [%d] not support yet\n", channel_id );
        return -1;
    }
    if( seq < 0 || seq >= g_cache_frm_seq[channel_id] ) {
        err("seq [%lld] out of range. current seq max [%lld]\n", seq, g_cache_frm_seq[channel_id] );
        return -1;
    }

    /// avoid one line too long 
    queue_t * q = queue_head( &g_cache_frm_queue[channel_id] );
    for( ; q != queue_tail( &g_cache_frm_queue[channel_id] ); q = queue_next(q) ) {
        sys_cache_frm_t * frm = ptr_get_struct( q, sys_cache_frm_t, queue );
        if( frm->seq == seq ) {
            /// forward traserval find previously idr frame
            queue_t * t = queue_prev(q);
            for( ; t != queue_tail( &g_cache_frm_queue[channel_id] ); t = queue_prev(t) ) {
                sys_cache_frm_t * frmm = ptr_get_struct( t, sys_cache_frm_t, queue );
                if( frmm->typ == 1 ) {
                    return frmm->seq;
                }
            }
        }
    }
    err("sys precv idr by specify not found\n");
    return -1;
}

/// @brief find the last video iframe seq number of specify channel
/// @param channel_id [IN] specify channel. 0:main channel 1:sub channel
/// @return seq number of last video idr frame, needs to use quickly to avoid seq outdated
long long sys_cache_last_idr( int channel_id )
{
    /// forward traserval, find the last dir frame seq number 
    if( channel_id < 0 || channel_id > 1 ) {
        err("channel_id [%d] not support yet\n", channel_id );
        return -1;
    }
    
    queue_t * q = queue_prev( &g_cache_frm_queue[channel_id] );
    for( ; q != queue_tail(&g_cache_frm_queue[channel_id]); q = queue_prev( q ) ) {
        sys_cache_frm_t * frm = ptr_get_struct( q, sys_cache_frm_t, queue );
        if( frm->typ == 1 ) {
            return frm->seq;
        }
    }
    return -1;
}


