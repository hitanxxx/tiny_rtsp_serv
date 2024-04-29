#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <malloc.h>
#include <semaphore.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/in.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sched.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>

#include "HM2P_Common.h"
#include "HM2P_Network.h"
#include "event.h"

ev_t * ev_find( ev_ctx_t * ctx, int fd )
{
    ev_t * ev = NULL;
    queue_t * q = queue_head( &ctx->queue );
    for( ; q != queue_tail(&ctx->queue); q = queue_next(q) ) {
        ev = ptr_get_struct( q, ev_t, queue);
        if( ev->fd == fd ) {
            return ev;
        }
    }
    return NULL;
}

void ev_timer_del( ev_ctx_t * ctx, int fd )
{
    ev_t * ev = ev_find(ctx, fd);
    if(ev) {
    	ev->exp_cb = NULL;
	    ev->exp_ts = 0;
    }
    return;
}

void ev_timer_add( ev_ctx_t * ctx, int fd, void * user_data, ev_exp_cb cb, int delay_msec )
{
    ev_t * ev = ev_find(ctx, fd);
    if(ev) {
    	ev->ext_data = user_data;
	    ev->exp_cb = cb;
	    ev->exp_ts = sys_ts_msec() + delay_msec;
    }
    return;
}

/// @brief add/del/mode fd with evt mgr  
/// @param evt [IN] evt mgr
/// @param fd [IN] fd 
/// @param ext [IN] fd extra data
/// @param cb [IN] fd callbacks
/// @param want_opt [IN] want trigger type
void ev_opt( ev_ctx_t * ctx, int fd, void * user_data, ev_cb cb, int op )
{
    /// some assert
    assert( fd >= 0 );
    assert( ctx != NULL );
    assert( op <= EV_RW );
    assert( op >= EV_NONE );

    /// find the fd form ev obj
    ev_t * ev = ev_find(ctx, fd);    
    if( ev ) {
        ev->cb = cb;
        ev->ext_data = user_data;
    
        if( ev->op != op ) {
            if( op == EV_NONE ) {
                ev->active = 0;
                FD_CLR( fd, &ctx->cache_rfds);
                FD_CLR( fd, &ctx->cache_wfds);
            } else if ( op == EV_RW ) {
                ev->active = 1;
                FD_SET( fd, &ctx->cache_rfds );
                FD_SET( fd, &ctx->cache_wfds );
            } else if ( op == EV_R) {
                ev->active = 1;
                FD_CLR( fd, &ctx->cache_wfds );
                FD_SET( fd, &ctx->cache_rfds );
            } else if ( op == EV_W ) {
                ev->active = 1;
                FD_CLR( fd, &ctx->cache_rfds);
                FD_SET( fd, &ctx->cache_wfds);
            }
            ev->op = op;
        }
    } else {
        /// ev_obj not find 
        if( op != EV_NONE ) {
            ev = sys_alloc(sizeof(ev_t));
            if( !ev ) {
                err("ev alloc fialed. [%d] [%s]\n", errno, strerror(errno));
                return;
            }
            ev->fd = fd;
            ev->cb = cb;
            ev->exp_ts = 0;
            ev->exp_cb = NULL;
            ev->ext_data = user_data;
            ev->ctx = ctx;
            queue_insert_tail( &ctx->queue, &ev->queue );
        	
	    ev->active = 1;
            if ( op == EV_RW ) {
                FD_SET( fd, &ctx->cache_rfds );
                FD_SET( fd, &ctx->cache_wfds );
            } else if ( op == EV_R) {
                FD_CLR( fd, &ctx->cache_wfds );
                FD_SET( fd, &ctx->cache_rfds );
            } else if ( op == EV_W ) {
                FD_CLR( fd, &ctx->cache_rfds);
                FD_SET( fd, &ctx->cache_wfds);
            }
            ev->op = op;
        }
    }
    return;
}

void ev_loop( ev_ctx_t * ctx )
{
    /*
    	round-robin check actions
    */
    int max_fd = -1;
    int actall = 0;
    unsigned long long cur_msec = sys_ts_msec();

    fd_set rfds;
    fd_set wfds;

    ///timer degree : 88 msecond
    struct timeval ts;
    memset( &ts, 0, sizeof(ts) );
    ts.tv_sec = 0;
    ts.tv_usec = 88 * 1000; 
  
    ev_t * ev = NULL;
    queue_t * q = queue_head( &ctx->queue );
    for( ; q != queue_tail(&ctx->queue); q = queue_next(q) ) {
        ev = ptr_get_struct( q, ev_t, queue);
        if( ev->active && (ev->fd > max_fd) ) {
            max_fd = ev->fd;
        }

        if( ev->exp_ts > 0 ) {
            if( cur_msec >= ev->exp_ts ) {
                dbg("ev fd [%d] timeout\n", ev->fd );
                if( ev->exp_cb ) ev->exp_cb( ctx, ev->fd, ev->ext_data );
                ev->exp_ts = 0;
            }
        }
    }

    memcpy( &rfds, &ctx->cache_rfds, sizeof(fd_set) );
    memcpy( &wfds, &ctx->cache_wfds, sizeof(fd_set) );

    actall = select( max_fd + 1, &rfds, &wfds, NULL, &ts );
    if( actall < 0 ) {
    	err("ev select failed. [%d]\n", errno );
	return;
    } else if (actall == 0 ) {
	///timeout. do nothing
	return;
    } else {
        int actn = 0;

        queue_t * q = queue_head( &ctx->queue );
        queue_t * n = NULL;
        while( q != queue_tail(&ctx->queue) ) {
            n = queue_next(q);

            ev_t * ev = ptr_get_struct( q, ev_t, queue);
            if( !ev->active ) {
                dbg("ev fd [%d] disactive.\n", ev->fd );
                queue_remove(q);
                sys_free(ev);
            } else {
                int rw = 0;
                if( FD_ISSET( ev->fd, &rfds ) ) {
                    rw |= EV_R;
                }
                if( FD_ISSET( ev->fd, &wfds ) ) {
                    rw |= EV_W;
                }
                if( rw > 0 ) {
                    if( ev->cb ) ev->cb( ctx, ev->fd, ev->ext_data, rw);
                    actn++;
                    if( actn >= actall ) {
                        break;
                    }
                }
            }
            q = n;
        }
    }
    return;
}

/// @brief create a evt mgr 
/// @param evt [IN]
/// @return 
int ev_create( ev_ctx_t ** ctx )
{
    ev_ctx_t * nctx = sys_alloc(sizeof(ev_ctx_t));
    if( !nctx ) {
        err("ev ctx alloc failed. [%d]\n", errno );
        return -1;
    }

    FD_ZERO(&nctx->cache_rfds);
    FD_ZERO(&nctx->cache_wfds);
    queue_init( &nctx->queue );
    *ctx = nctx;
    return 0;
}

/// @brief free a evt mgr
/// @param evt 
void ev_free( ev_ctx_t * ctx )
{
    if ( ctx ) {
        FD_ZERO( &ctx->cache_rfds );
        FD_ZERO( &ctx->cache_wfds );
        sys_free(ctx);
    }
}


