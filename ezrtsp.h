#ifndef __EZRTSP_H__
#define __EZRTSP_H__

#include "HM2P_Common.h"

/// evt datas 
#define EV_FD_MAX  1024

#define EV_NONE	0x0
#define EV_R	0x1
#define EV_W	0x2
#define EV_RW	0X3

typedef struct evt evt_t;
typedef struct evt_obj evt_obj_t;
typedef void ( * evt_cb ) ( evt_obj_t * ev, int trigger_type );
typedef void ( * evt_timeout_cb ) ( evt_obj_t * ev );

/// @brief evt obj data
struct evt_obj 
{
    queue_t     queue;
    evt_t *     evt;
    evt_cb      cb;
    int         fd;
    int     active;
    int     opt;
    void *      ext_data;
    int         expire_time;
    evt_timeout_cb  ev_timeout_cb;
};

/// @brief evt mgr datas
struct evt
{
    fd_set cache_rfds;
    fd_set cache_wfds;
    queue_t queue;
};

/// evt api
int evt_create( evt_t ** evt );
void evt_free( evt_t * evt );
void evt_loop( evt_t * evt );
void evt_opt( evt_t * evt, int fd, void * ext, evt_cb cb, int want_opt );
void evt_timer_add( evt_obj_t * ev, evt_timeout_cb timeout_cb, int sec );
void evt_timer_del( evt_obj_t * ev );
evt_obj_t * evt_find( evt_t * evt, int fd );


// ez rtsp
int ezrtsp_start( );
int ezrtsp_video_paramset_clear( );
int ezrtsp_video_paramset_save( int chn, struct video_stream * stream );



#endif

