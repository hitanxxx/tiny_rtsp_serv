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

/// @brief delete the timer of fd
/// @param ev 
void evt_timer_del( evt_obj_t * ev )
{
    dbg("\n");
    if( ev->expire_time > 0 ) {
        ev->expire_time = 0;
    }
    return;
}

/// @brief add the timer of fd 
/// @param ev 
/// @param timeout_cb 
/// @param sec 
void evt_timer_add( evt_obj_t * ev, evt_timeout_cb timeout_cb, int sec )
{
    dbg("\n");
    ev->ev_timeout_cb   = timeout_cb;
    ev->expire_time = time(NULL) + sec;
    return;
}


evt_obj_t * evt_find( evt_t * evt, int fd )
{
    evt_obj_t * ev_obj = NULL;
    queue_t * q = queue_head( &evt->queue );
    for( ; q != queue_tail(&evt->queue); q = queue_next(q) ) {
        ev_obj = ptr_get_struct( q, evt_obj_t, queue);
        if( ev_obj->fd == fd ) {
            return ev_obj;
        }
    }
    return NULL;
}


/// @brief add/del/mode fd with evt mgr  
/// @param evt [IN] evt mgr
/// @param fd [IN] fd 
/// @param ext [IN] fd extra data
/// @param cb [IN] fd callbacks
/// @param want_opt [IN] want trigger type
void evt_opt( evt_t * evt, int fd, void * ext, evt_cb cb, int want_opt )
{
    /// some assert
    assert( want_opt <= EV_RW );
    assert( fd >= 0 );
    assert( evt != NULL );

    /// find the fd form ev obj
    evt_obj_t * ev_obj = evt_find(evt, fd);    
    if( ev_obj ) {
        ev_obj->cb = cb;
        ev_obj->expire_time = 0;
        ev_obj->ev_timeout_cb = NULL;
        ev_obj->ext_data = ext;
    
        if( ev_obj->opt != want_opt ) {
            if( want_opt == EV_NONE ) {
                if( ev_obj->opt & EV_R ) {
                    FD_CLR( fd, &evt->cache_rfds);
                }
                if( ev_obj->opt & EV_W ) {
                    FD_CLR( fd, &evt->cache_wfds);
                }
                ev_obj->active = 0;
                
            } else {
            
                if( want_opt & EV_R && want_opt & EV_W ) {
                    if( !(ev_obj->opt & EV_R) ) {
                        FD_SET( fd, &evt->cache_rfds );
                    }
                    if( !(ev_obj->opt & EV_W) ) {
                        FD_SET( fd, &evt->cache_wfds );
                    }
                    ev_obj->active = 1;
                } else if ( want_opt & EV_R ) {
                    if( ev_obj->opt & EV_W ) {
                        FD_CLR( fd, &evt->cache_wfds );
                    }
                    if( !(ev_obj->opt & EV_R) ) {
                        FD_SET( fd, &evt->cache_rfds );
                    }
                    ev_obj->active = 1;
                } else if ( want_opt & EV_W ) {
                    if( ev_obj->opt & EV_R ) {
                        FD_CLR( fd, &evt->cache_rfds);
                    }
                    if( !(ev_obj->opt & EV_W) ) {
                        FD_SET( fd, &evt->cache_wfds);
                    }
                    ev_obj->active = 1;
                }
                ev_obj->opt = want_opt;
            }
        }
    } else {
        /// ev_obj not find 
        if( want_opt == EV_NONE ) {
            /// do nothing 
        } else {
            ev_obj = sys_alloc(sizeof(evt_obj_t));
            ev_obj->fd = fd;
            ev_obj->cb = cb;
            ev_obj->expire_time = 0;
            ev_obj->ev_timeout_cb = NULL;
            ev_obj->ext_data = ext;
            ev_obj->evt = evt;
            
            queue_insert_tail( &evt->queue, &ev_obj->queue );
        
            if( want_opt & EV_R && want_opt & EV_W ) {
                if( !(ev_obj->opt & EV_R) ) {
                    FD_SET( fd, &evt->cache_rfds );
                }
                if( !(ev_obj->opt & EV_W) ) {
                    FD_SET( fd, &evt->cache_wfds );
                }
                ev_obj->active = 1;
            } else if ( want_opt & EV_R ) {
                
                if( !(ev_obj->opt & EV_R) ) {
                    FD_SET( fd, &evt->cache_rfds );
                }
                ev_obj->active = 1;
            } else if ( want_opt & EV_W ) {
                
                if( !(ev_obj->opt & EV_W) ) {
                    FD_SET( fd, &evt->cache_wfds);
                }
                ev_obj->active = 1;
            }

            ev_obj->opt = want_opt;
        }
    }
    return;
}

void evt_loop( evt_t * evt )
{
    /*
    current use round-robin method to check fd actions
    round-robin timeout is 100 msecond
    */
    int max_fd = -1;
    int actall = 0;
    int cur_sec = 0;

    fd_set rfds;
    fd_set wfds;

    /// set run loop interval 50 msecond
    struct timeval select_interval;
    memset( &select_interval, 0, sizeof(struct timeval) );
    select_interval.tv_sec	= 0;
    select_interval.tv_usec = 50 * 1000;
    
    cur_sec = time(NULL);

    evt_obj_t * ev_obj = NULL;
    queue_t * q = queue_head( &evt->queue );
    for( ; q != queue_tail(&evt->queue); q = queue_next(q) ) {
        ev_obj = ptr_get_struct( q, evt_obj_t, queue);
        if( ev_obj->fd > 0 && ev_obj->fd > max_fd ) {
            max_fd = ev_obj->fd;
        }

        if( ev_obj->expire_time > 0 ) {
            if( cur_sec > ev_obj->expire_time ) {
                dbg("ev fd [%d] timeout\n", ev_obj->fd );
                if( ev_obj->ev_timeout_cb ) {
                    ev_obj->ev_timeout_cb( ev_obj );
                }
                ev_obj->expire_time = 0;
            }
        }
    }

    memcpy( &rfds, &evt->cache_rfds, sizeof(fd_set) );
    memcpy( &wfds, &evt->cache_wfds, sizeof(fd_set) );

    actall = select( max_fd + 1, &rfds, &wfds, NULL, &select_interval );
    if( actall <= 0 ) {
        //if( ret == 0 ) err("select timeout\n");
        return;
    } else {

        int actn = 0;
        evt_obj_t * ev_obj = NULL;
        queue_t * q = queue_head( &evt->queue );
        for( ; q != queue_tail(&evt->queue); q = queue_next(q) ) {
            ev_obj = ptr_get_struct( q, evt_obj_t, queue);
            
            int typ = 0;
            if( !ev_obj->active ) {
                continue;
            }
            if( FD_ISSET( ev_obj->fd, &rfds ) ) {
                typ |= EV_R;
            }
            if( FD_ISSET( ev_obj->fd, &wfds ) ) {
                typ |= EV_W;
            }
            if( (typ > 0) && ev_obj->cb ) {
                actn ++;
                ev_obj->cb(ev_obj, typ);
            }
            if( actn >= actall ) {
                break;
            }
        }

        q = queue_head( &evt->queue );
        queue_t * n = NULL;
        while( q != queue_tail(&evt->queue) ) {
            n = queue_next(q);

            ev_obj = ptr_get_struct( q, evt_obj_t, queue);
            if( !ev_obj->active ) {
                dbg("fd [%d] not active. free it\n", ev_obj->fd );
                queue_remove(q);
                sys_free(ev_obj);
            }
            
            q = n;
        }
    }
    return;
}

/// @brief create a evt mgr 
/// @param evt [IN]
/// @return 
int evt_create( evt_t ** evt )
{
    evt_t * n_evt = sys_alloc(sizeof(evt_t) );
    if( !n_evt ) {
        err("alloc evt failed. [%d]\n", errno );
        return -1;
    }

    FD_ZERO(&n_evt->cache_rfds);
    FD_ZERO(&n_evt->cache_wfds);
    queue_init( &n_evt->queue );
    
    *evt = n_evt;
    return 0;
}

/// @brief free a evt mgr
/// @param evt 
void evt_free( evt_t * evt )
{
    if ( evt ) {
        /// clear fdset 
        FD_ZERO( &evt->cache_rfds );
        FD_ZERO( &evt->cache_wfds );

        /// transverl the queue and clear it 
        sys_free(evt);
        evt = NULL;
    }
}


