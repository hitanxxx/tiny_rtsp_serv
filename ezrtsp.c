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

#include "hardware_platform.h"
#include "HM2P_Network.h"
#include "HM2P_Video.h"
#include "ezcache.h"
#include "ezrtsp.h"


#define METHOD_OPTIONS	    1
#define METHOD_DESCRIBE     2
#define METHOD_SETUP        3
#define METHOD_PLAY         4


#define EZRTSP_INIT 0x1

static pthread_t ez_rtsp_task_pid;
static int ez_rtsp_stat = 0;


/// @brief rtsp connection info
typedef struct conn
{
    int fd;
    evt_t * ev;

    char buffer[4096];          
	char * pos;
	char * last;
	char * start;
	char * end;

    char client_ip[64];
    int req_complete;
    int req_cseq;
    int req_method;
    int req_setup_tcp;

    /// @brief rtp threads
    int         rtp_stat;
    pthread_t   rtp_pid;

    /// 
    int         rtp_seq;
    int         rtp_ts;
    unsigned long long rtp_ts_frist;
} rtsp_con_t;


static int g_listen_fd = 0;


/// data for storge video paramsets data: pps/vps/sps etc
static char* g_video_vps[2] = {NULL};
static int g_video_vpsn[2] = {0};
static char *g_video_sps[2] = {NULL};
static int g_video_spsn[2] = {0};
static char *g_video_pps[2] = {NULL};
static int g_video_ppsn[2] = {0};
static int g_ps_stat[2] = {0};


void ezrtsp_process_request( evt_obj_t * ev, int trigger_type );


#define PUT_16(p, v) ((p)[0] = ((v) >> 8) & 0xff, (p)[1] = (v)&0xff)
#define PUT_32(p, v) ((p)[0] = ((v) >> 24) & 0xff, (p)[1] = ((v) >> 16) & 0xff, (p)[2] = ((v) >> 8) & 0xff, (p)[3] = (v) & 0xff)
#define GET_16(p)    (((p)[0] << 8) | (p)[1])
#define GET_32(p)    (((p)[0] << 24) | ((p)[1] << 16) | ((p)[2] << 8) | (p)[3])


static const char *ffmpeg_find_nal_uint_interval(const char *p, const char *end)
{
    const char *a = p + 4 - ((int)p & 3);

    for (end -= 3; p < a && p < end; p++)
    {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for (end -= 3; p < end; p += 4)
    {
        int x = *(const int *)p;
        if ((x - 0x01010101) & (~x) & 0x80808080)   // generic
        {
            if (p[1] == 0)
            {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p + 1;
            }
            if (p[3] == 0)
            {
                if (p[2] == 0 && p[4] == 1)
                    return p + 2;
                if (p[4] == 0 && p[5] == 1)
                    return p + 3;
            }
        }
    }

    for (end += 3; p < end; p++)
    {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

const char *ffmpeg_find_nal_uint(const char *p, const char *end)
{
    // dbg("\n");
    const char *out = ffmpeg_find_nal_uint_interval(p, end);
    if (p < out && out < end && !out[-1])
    {
        out--;
    }
    return out;
}

int ezrtsp_video_paramset_clear( )
{
    /// set clear flag
    int chn = 0;
    for( chn = 0; chn < 2; chn ++ ) {
        if( g_video_vps[chn] ) {
            sys_free( g_video_vps[chn] );
            g_video_vps[chn] = NULL;
        } else {
            err("video chn [%d] vps null\n", chn );
        }
        if( g_video_sps[chn] ) {
            sys_free( g_video_sps[chn] );
            g_video_sps[chn] = NULL;
        } else {
            err("video chn [%d] sps null\n", chn );
        }
        if( g_video_pps[chn] ) {
            sys_free( g_video_pps[chn] );
            g_video_pps[chn] = NULL;
        } else {
            err("video chn [%d] pps null\n", chn );
        }
        g_ps_stat[chn] = 0;    
    }
    return 0;
}

int ezrtsp_video_paramset_get( int chn, char * vps, int *vpsn, char * sps, int *spsn, char * pps, int *ppsn )
{
    /// get info
    if( g_ps_stat[chn] == 1 ) {
        if( vps ) {
            if( g_video_vpsn[chn] ) {
                *vpsn = g_video_vpsn[chn];
                memcpy( vps, g_video_vps[chn], *vpsn );
            } else {
                vpsn = 0;
            }
        }
        if( sps ) {
            if( g_video_spsn[chn] ) {
                *spsn = g_video_spsn[chn];
                memcpy( sps, g_video_sps[chn], *spsn );
            } else {
                spsn = 0;
            }
        }
        if( pps ) {
            if( g_video_ppsn[chn] ) {
                *ppsn = g_video_ppsn[chn];
                memcpy( pps, g_video_pps[chn], *ppsn );
            } else {
                ppsn = 0;
            }
        }
    }
    return 0;
}

int ezrtsp_video_paramset_save( int chn, struct video_stream * stream )
{
    /// save info
    if( g_ps_stat[chn] == 0 ) {
        if( stream->frame_type == FRAME_TYPE_I ) {
            /// tarversal find nalu in the stream data
            const char * p = (char*)stream->data;
            const char * end = p + stream->len;
            const char * nal_start = NULL, *nal_end = NULL;

            nal_start = ffmpeg_find_nal_uint( p, end );
            for(;;) {
                int nlen = 0;
                while( nal_start < end && !*(nal_start++) ) {
                    ;
                }
                if( nal_start == end )
                    break;

                nal_end = ffmpeg_find_nal_uint( nal_start, end );
                nlen = nal_end - nal_start;

                if( video_get_enctype() == 1 ) {
                    /// h265 
                    int nalu_type = (nal_start[0]>>1) & 0x3f;
                    if ( nalu_type == 32 ) {
                        /// iframe nal uint type VPS
                        if( g_video_vps[chn] == NULL ) {
                            g_video_vpsn[chn] = nlen;
                            g_video_vps[chn] = sys_alloc(g_video_vpsn[chn] );
                            if( g_video_vps[chn] ) {
                                memcpy( g_video_vps[chn], nal_start, g_video_vpsn[chn] );
                            }
                        } else {
                            err("video chn [%d] vps not null\n", chn );
                        }
                    } else if ( nalu_type == 33 ) {
                        /// iframe nal uint type SPS
                        if( g_video_sps[chn] == NULL ) {
                            g_video_spsn[chn] = nlen;
                            g_video_sps[chn] = sys_alloc(g_video_spsn[chn] );
                            if( g_video_sps[chn] ) {
                                memcpy( g_video_sps[chn], nal_start, g_video_spsn[chn] );
                            }
                        } else {
                            err("video chn [%d] sps not null\n", chn );
                        }
                    } else if ( nalu_type == 34 ) {
                        /// iframe nal uint type PPS
                        if( g_video_pps[chn] == NULL ) {
                            g_video_ppsn[chn] = nlen;
                            g_video_pps[chn] = sys_alloc(g_video_ppsn[chn] );
                            if( g_video_pps[chn] ) {
                                memcpy( g_video_pps[chn], nal_start, g_video_ppsn[chn] );
                            }
                        } else {
                            err("video chn [%d] pps not null\n", chn );
                        }
                    }
                } else {
                    int nalu_type = nal_start[0]&0x1f;
                    
                    if( nalu_type == 7 ) {
                        /// sps 
                        if( g_video_sps[chn] == NULL ) {
                            g_video_spsn[chn] = nlen;
                            g_video_sps[chn] = sys_alloc(g_video_spsn[chn] );
                            if( g_video_sps[chn] ) {
                                memcpy( g_video_sps[chn], nal_start, g_video_spsn[chn] );
                            }
                        } else {
                            err("video chn [%d] sps not null\n", chn );
                        }
                    } else if ( nalu_type == 8 ) {
                        /// pps
                        if( g_video_pps[chn] == NULL ) {
                            g_video_ppsn[chn] = nlen;
                            g_video_pps[chn] = sys_alloc(g_video_ppsn[chn] );
                            if( g_video_pps[chn] ) {
                                memcpy( g_video_pps[chn], nal_start, g_video_ppsn[chn] );
                            }
                        } else {
                            err("video chn [%d] pps not null\n", chn );
                        }
                    }
                }
                
                nal_start = nal_end;
            } 
            g_ps_stat[chn] = 1;       
        }
    }
    return 0;
}




int ezrtp_packet_send( int fd, char * data, int len )
{
    int sendn = 0;

    while( sendn < len ) {
        int rc = send( fd, data + sendn, len - sendn, 0 );
        if( rc < 0 ) {
            if( errno == EAGAIN ) {
                err("rtp send EAGAIN, continue\n");
                continue;
            }
            err("rtp send failed. [%d]\n", errno );
            return -1;
        }
        sendn += rc;
    }
    return 0;
}

int ezrtp_packet_build( rtsp_con_t * c, int payload, const char * data, int size, int marker, unsigned long long ts )
{
    int send_len = 0;
    char rtp_packet[1500] = {0};
    char * pack = rtp_packet;

    if(1) {
        /// RTP over tcp, (RTSP Interleaved Frame)
        ///  | char | char | short |
        pack[0] = '$';                      // 0x24
        pack[1] = 0x00;                     // 0x00 
        PUT_16( pack + 2, (12 + size) );    // rtp data length
        send_len += 4;
    }

    if( c->rtp_ts_frist == 0 ) {
        c->rtp_ts_frist = ts;
    }    

    /// RTP header 
    char * rtp_hdr = NULL;
    rtp_hdr = rtp_packet + send_len;
    rtp_hdr[0] = 2 << 6;   // fixed 
    rtp_hdr[1] = (payload &0x7f) | ((marker&0x01)<<7); // fixed
    PUT_16( rtp_hdr + 2, c->rtp_seq );  /// sequence
    PUT_32( rtp_hdr + 4, ts - c->rtp_ts_frist ); // PTS
    PUT_32( rtp_hdr + 8, 0x252525 );  /// SSRC
    send_len += 12;

    /// RTP payload 
    memcpy( (rtp_packet + send_len), data, size );
    send_len += size;

    /// send RTP over TCP
    if( 0 != ezrtp_packet_send( c->fd, rtp_packet, send_len ) ) {
        return -1;
    }
    c->rtp_seq += 1;
    return 0;
}

int ezrtp_fu_264( rtsp_con_t * c, int payload, char * nal, int len, int mark, unsigned long long ts )
{
    int ret = 0;
    char * fua = NULL;
    char pkt[1500] = {0};
    int pktlen = 0;

    pkt[0] = 28 | ( nal[0] & 0xe0 );
    pkt[1] = 0x80 | (nal[0] & 0x1f );

    /// skip nal hdr 
    fua = nal+1;
    len -= 1;

    while( (len + 2) > 1462 ) {
        pktlen = 1462 - 2;
        memcpy( &pkt[2], fua, pktlen );
        
        if( ( ret = ezrtp_packet_build( c, payload, pkt, 1462, 0, ts ) ) < 0 ) {
            return ret;
        }

        fua += pktlen;
        len -= pktlen;
        pkt[1] &= ~0x80;
    }
    pkt[1]  |= 0x40;
    memcpy( &pkt[2], fua, len );
    return ezrtp_packet_build(c, payload, pkt, (len+2), 1, ts );
}

int ezrtp_fu_265( rtsp_con_t * c, int payload, char * nal, int len, int mark, unsigned long long ts )
{
    char * fua = NULL;
    char pkt[1500] = {0};
    int pktlen = 0;
    int ret = -1;

    /// fix H265 FU start header

    /// pkt[0] pkt[1] means HEVC NALU header
    /// FU nalu header, type must be 49
    pkt[0] = 49<<1;
    pkt[1] = 1;
    /// pkt[2] means 1 BYTE FU header
    pkt[2] = 0x80 | ( (nal[0] >> 1) & 0x3f );

    /// delete 2 byte for HEVC nal uint
    fua = nal + 2;
    len -= 2;

    /// devide H265 data to small pieces and send 
    while( (len + 3) > 1462 ) {
        // dbg("len [%d]\n", len );
        pktlen = 1462 - 3;
        memcpy( &pkt[3], fua, pktlen );

        if( ( ret = ezrtp_packet_build( c, payload, pkt, 1462, 0, ts ) ) < 0 ) {
            return ret;
        }
        
        fua += pktlen;
        len -= pktlen;

        /// after send FU header, reset FU header to not START mark
        pkt[2] &= ~0x80;
    }
    /// set FU header to END mark
    pkt[2] |= 0x40;
    memcpy( &pkt[3], fua, len );
    return ezrtp_packet_build( c, payload, pkt, (len+3), 1, ts );
}

int ezrtp_send_frame( rtsp_con_t * c, char * frame_data, int frame_len, unsigned long long ts )
{
    int ret = 0;
    const char * p = frame_data;
    const char * end = p + frame_len;
    const char * nal_start = NULL, *nal_end = NULL;

    ts = 90000*ts/1000;

    /// 1.frist find nal uint form frame data.
    nal_start = ffmpeg_find_nal_uint( p, end );
    for(;;) {
        int nlen = 0;
        while( nal_start < end && !*(nal_start++) ) {
            ;
        }
        if( nal_start == end )
            break;

        nal_end = ffmpeg_find_nal_uint( nal_start, end );
        nlen = nal_end - nal_start;
        
        /// 2. pack nal uint to rtp format with different way  

        /// if nal uint length < mtu
        /// send rtp with single nal uint 
        if( nlen <= 1462 ) {
            if( video_get_enctype() == 0 ) {
                ret = ezrtp_packet_build( c, 96, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1, ts );
            } else {
                ret = ezrtp_packet_build( c, 97, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1, ts );
            }
        } else {
            /// if nal uint length > mtu
            /// goto rtp send with fragmentation uint
            if( video_get_enctype() == 0 ) {
                ret = ezrtp_fu_264( c, 96, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1, ts );
            } else {
                ret = ezrtp_fu_265( c, 97, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1, ts );
            }
        }

        if( ret < 0 ) {
            break;
        }

        nal_start = nal_end;
    }
    return ret;
}



int ezrtsp_con_alloc( rtsp_con_t ** c )
{
    rtsp_con_t * n = sys_alloc(sizeof(rtsp_con_t) );
    if( !n ) {
        err("rtsp con alloc failed. [%d] [%s]\n", errno, strerror(errno) );
        return -1;
    }
	n->start = n->pos = n->last = n->buffer;
	n->end = n->start + sizeof(n->buffer);
    
    *c = n;
    return 0;
}

void ezrtsp_con_free( rtsp_con_t * c )
{
	if( c ) {
		if( c->fd > 0 ) {

            dbg("del timer\n");
            evt_obj_t * ev_obj = evt_find(c->ev, c->fd);
            if( ev_obj ) {
                evt_timer_del( ev_obj );
            }
            dbg("evt close\n");
			evt_opt( c->ev, c->fd, NULL, NULL, EV_NONE );
            if( c->rtp_stat == 1 ) {
                c->rtp_stat = 0;
                pthread_join( c->rtp_pid, NULL );
            }
            dbg("stop rtp task if have\n");
			close(c->fd);
		}			
		sys_free(c);
        dbg("free c\n");
	}	
    dbg("return\n");
}

void ezrtsp_timeout_con( evt_obj_t * ev )
{
    rtsp_con_t * con = (rtsp_con_t*) ev->ext_data;
    ezrtsp_con_free( con );
}

void ezrtsp_resp_send( evt_obj_t * ev, int trigger_type )
{
    int sent = 0;
    rtsp_con_t * c = (rtsp_con_t *)ev->ext_data;
    assert( c != NULL );
    dbg("\n");
    while( c->pos < c->last ) {
        sent = send( c->fd, c->pos, c->last - c->pos, 0 );
        if( sent <= 0 ) {
            if( (errno == EAGAIN) || (errno == EWOULDBLOCK) ) {
                evt_timer_add( ev, ezrtsp_timeout_con, 5 ); 
                return;
            }
            err("rtsp con resp send error. [%d] [%s]\n", errno, strerror(errno) );
            ezrtsp_con_free( c );
            return;
        }
        c->pos += sent;
    }
    dbg("rtsp con resp send [%d] [%s]\n", c->pos - c->start, c->start );
    evt_timer_del( ev );

    memset( c->buffer, 0, sizeof(c->buffer) );
    c->start = c->pos = c->last = c->buffer;
    c->end = c->start + sizeof(c->buffer);

    c->req_complete = 0;
    c->req_method = 0;
    c->req_cseq = 0;

    evt_opt( ev->evt, c->fd, (void*)c, ezrtsp_process_request, EV_R );
}


int ezrtsp_resp_sdp( char * str )
{
    int vpsn = 0, spsn = 0, ppsn = 4;
	char vps[128] = {0}, sps[512] = {0}, pps[128] = {0};
	char vpsb64[128] = {0}, spsb64[512] = {0}, ppsb64[128] = {0};

    if( video_get_enctype() == 0 ) {
        ezrtsp_video_paramset_get( 1, NULL, NULL, sps, &spsn, pps, &ppsn );
        
        int level = (sps[1] << 16) | (sps[2] << 8) | sps[3];
        sys_base64_encode(sps, spsn, spsb64, sizeof(spsb64));
        sys_base64_encode(pps, ppsn, ppsb64, sizeof(ppsb64));

        sprintf( str, "profile-level-id=%06X;sprop-parameter-sets=%s,%s", level, spsb64, ppsb64);
    } else {
        ezrtsp_video_paramset_get( 1, vps, &vpsn, sps, &spsn, pps, &ppsn );
        sys_base64_encode(vps, vpsn, vpsb64, sizeof(vpsb64));
        sys_base64_encode(pps, ppsn, ppsb64, sizeof(ppsb64));
        sys_base64_encode(sps, spsn, spsb64, sizeof(spsb64));
        sprintf( str, "sprop-vps=%s;sprop-sps=%s;sprop-pps=%s", vpsb64, spsb64, ppsb64);
    }
	return 0;
}

static void * ezrtp_send_task( void * para )
{
    rtsp_con_t * c = (rtsp_con_t*)para;

    char task_name[32] = {0};
    sprintf( task_name, "hm2p_ezrtp%d", c->fd );
    SET_THREAD_NAME(task_name);

    long long chn_seq = -1;

    while( c->rtp_stat == 1 ) {

        if( chn_seq == -1 ) {
            chn_seq = ezcache_last_idr( 1 );
            if( chn_seq == -1 ) {
                sys_msleep(10);
                continue;
            }
        }

        ezcache_frm_t * frm = ezcache_frm_get( 1, chn_seq );
        if( frm ) {
            chn_seq++;
            /// goto send         
            ezrtp_send_frame( c, (char*)frm->data, frm->datan, frm->ts );
            /// goto free
            sys_free(frm);
        } else {
            /// goto wait 
            sys_msleep(2);
        }        
    }
    return NULL;
}

void ezrtsp_resp( evt_obj_t * ev, int trigger_type )
{
    dbg("\n");
    /// get device ipaddress
    char wireless_ip[64] = {0};
    net_ifname_ip_get( IFNAME_WLAN, wireless_ip );

    rtsp_con_t * c = (rtsp_con_t *)ev->ext_data;
    assert( c != NULL );

    /// reset buffer
    memset( c->buffer, 0, sizeof(c->buffer) );
    c->start = c->pos = c->last = c->buffer;
    c->end = c->start + sizeof(c->buffer);
    
    if( c->req_method ==  METHOD_OPTIONS ) {
        c->last += snprintf( c->buffer, sizeof(c->buffer),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Date: Thu, Jan 01 1970 00:00:00 GMT\r\n"
            "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n"
            "\r\n",
            c->req_cseq
        );
    } else if ( c->req_method == METHOD_DESCRIBE ) {
        ///build sdp string
        char sdp[1024] = {0};
        ezrtsp_resp_sdp( sdp );
        ///dbg("sdp string [%s]\n", sdp );

        /// build payload string 
        char payload[4096] = {0};
        int payload_len = 0;
        if( video_get_enctype() == 0 ) {
            payload_len = snprintf( payload, sizeof(payload)-1,
                "v=0\r\n"
                "s=ipcamera\r\n"
                "t=0 0\r\n"
                "a=control:*\r\n"
                "a=range:npt=0-\r\n"
                "a=recvonly\r\n"
                "a=control:*\r\n"
                "m=video 0 RTP/AVP 96\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "b=AS:1024\r\n"
                "a=rtpmap:96 H264/90000\r\n"
                "a=framerate: 15\r\n"
                "a=fmtp:96 packetization-mode=1;%s\r\n",
                sdp
            );
        } else {
            payload_len = snprintf( payload, sizeof(payload)-1,
                "v=0\r\n"
                "s=ipcamera\r\n"
                "t=0 0\r\n"
                "a=control:*\r\n"
                "a=range:npt=0-\r\n"
                "a=recvonly\r\n"
                "a=control:*\r\n"
                "m=video 0 RTP/AVP 97\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "b=AS:5000\r\n"
                "a=rtpmap:97 H265/90000\r\n"
                "a=framerate: 15\r\n"
                "a=fmtp:97 packetization-mode=1;%s\r\n",
                sdp
            );
        }
        /// build resp string 
        c->last += snprintf( c->buffer, sizeof(c->buffer)-1,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Date: Thu, Jan 01 1970 03:34:59 GMT\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            c->req_cseq,
            payload_len,
            payload
        );
    } else if ( c->req_method == METHOD_SETUP ) {
        if( c->req_setup_tcp == 0 ) {
            //// only support TCP type
            c->last += snprintf( c->buffer, sizeof(c->buffer),
                "RTSP/1.0 461 Unsupported transport\r\n"
                "CSeq: %d\r\n"
                "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
                "\r\n"
                ,
                c->req_cseq
            );
        } else {
            c->last += snprintf( c->buffer, sizeof(c->buffer),
                "RTSP/1.0 200 OK\r\n"
                "CSeq: %d\r\n"
                "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=252525;mode=\"play\"\r\n"
                "Session: F89623B6\r\n"
                "\r\n"
                ,
                c->req_cseq
            );
        }
    } else if ( c->req_method == METHOD_PLAY ) {
        
        c->last += snprintf( c->buffer, sizeof(c->buffer),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
            "Range: npt=0.000-\r\n"
            "Session: F89623B6\r\n"
            "RTP-Info: url=rtsp://%s:554/trackID=1;seq=9378;rtptime=4848\r\n"
            "\r\n"
            ,
            c->req_cseq,
            wireless_ip
        );
        // request play process
        // 1. return 200 OK RTSP response
        // 2. start a task to send video stream form sys cache 
        // 3. delete timer for the connection
        // 4. clear the meta, when recvid data

        if( c->rtp_stat != 1 ) {
            c->rtp_stat = 1;
            if( 0 != pthread_create( &c->rtp_pid, NULL, &ezrtp_send_task, (void*)c ) ) {
                err("ezrtsp send task create failed. [%d]\n", errno );
                return;
            }
        } else {
            err("ezrtp send task alread in running\n");
            return;
        }
    }
    evt_opt( ev->evt, c->fd, (void*)c, ezrtsp_resp_send, EV_W );
}

void ezrtsp_process_request( evt_obj_t * ev, int trigger_type )
{
    rtsp_con_t * c = (rtsp_con_t*)ev->ext_data;
    int recvd = 0;
    char * p = NULL;

    assert( EV_R == trigger_type );

	while( !c->req_complete ) {
        
		recvd = recv( c->fd, c->last, c->end - c->last, 0 );
		if( recvd <= 0 ) {
			if( recvd == 0 )  {
				err("rtsp con peer closed, free rtsp con\n");
				return ezrtsp_con_free( c );
			}			
			if( (errno == EAGAIN) || (errno == EWOULDBLOCK) ) {

                if( c->rtp_stat == 1 ) {
                    evt_timer_del( ev );
                    dbg("recv EAGAIN. (connection in playing.)\n");
                    /// reset the meta
                    memset( c->buffer, 0, sizeof(c->buffer) );
                    c->start = c->pos = c->last = c->buffer;
                    c->end = c->start + sizeof(c->buffer);
                } else {
                    evt_timer_add( ev, ezrtsp_timeout_con, 5 ); 
                }
				return;
			} else {
				err("rtsp con recv failed. free rtsp con, fd [%d] [%d] [%s]\n", c->fd, errno, strerror(errno) );
				return ezrtsp_con_free(c);
			}
		}
		c->last += recvd;
        // dbg("recvd len [%d]\n", recvd );
	
        /// find finish mark
		p = strstr( c->pos, "\r\n\r\n" );
		if( p ) {
			dbg("rtsp con req recvd complete\n[%s]\n", c->pos );
			c->req_complete = 1;
		}
	}

    /// find Cseq 
    p = strstr ( c->pos, "CSeq:" );
    if( p ) {
        c->req_cseq  = strtol( p + strlen("CSeq:"), NULL, 10 );
    } else {
        err("rtsp request not found [Cseq]\n");
        return ezrtsp_con_free(c);
    }

    /// find METHOD 
    if( strstr ( c->pos, "OPTIONS " ) != NULL ) {
        c->req_method = METHOD_OPTIONS;
    } else if ( strstr ( c->pos, "DESCRIBE " ) != NULL ) {
        c->req_method = METHOD_DESCRIBE;
    } else if ( strstr ( c->pos, "SETUP " ) != NULL ) {
        c->req_method = METHOD_SETUP;
        if( NULL == strstr( c->pos, "TCP" ) ) {
            c->req_setup_tcp = 0;
        } else {
            c->req_setup_tcp = 1;
        }
    } else if ( strstr ( c->pos, "PLAY " ) != NULL ) {
        c->req_method = METHOD_PLAY;
    } else {
        return ezrtsp_con_free( c );
    }
    
    evt_opt( ev->evt, c->fd, (void*)c, ezrtsp_resp, EV_W );
}

void ezrtsp_accept( evt_obj_t * ev, int trigger_type )
{
    int cfd = 0;
    rtsp_con_t * c = NULL;
    
    assert( EV_R == trigger_type );

    socklen_t len = 0;
    struct sockaddr_in caddr;
    memset( &caddr, 0, sizeof(struct sockaddr_in) );
    cfd = accept( ev->fd, (struct sockaddr*)&caddr, &len );
    if( cfd == -1 ) {
        err("ezrtsp accept cli failed. [%d]\n", errno );
        return;
    }
    if( 0 != ezrtsp_con_alloc(&c) ) {
        err("ezrtsp alloc cli failed\n");
        close(cfd);
        return;
    }
    c->fd = cfd;
    c->ev = ev->evt;
    int nbio = 1;
    ioctl( cfd, FIONBIO, &nbio );

    strncpy( c->client_ip, inet_ntoa(caddr.sin_addr), sizeof(c->client_ip) );
    dbg("ezrtsp accept new cli. addr [%p] fd [%d] ip [%s]\n", c, c->fd, c->client_ip );
    evt_opt( ev->evt, c->fd, (void*)c, ezrtsp_process_request, EV_R );

    //evt_obj_t * cli_obj = evt_find( ev->evt, c->fd );
    //return ezrtsp_process_request( cli_obj, EV_R );
}

void * ezrtsp_task( void * para )
{
    evt_t * ev = NULL;

    SET_THREAD_NAME("ezrtsp listen");
    
    dbg("ezrtsp task start\n");
    if( 0 != evt_create(&ev) ) {
        err("ev create failed\n");
        return NULL;
    }

    /// register accept callback for listen read event  
    evt_opt( ev, g_listen_fd, (void*)ev, ezrtsp_accept, EV_R );
    while( ez_rtsp_stat & EZRTSP_INIT ) {
        evt_loop(ev);
    }

    if( g_listen_fd > 0 ) {
        close(g_listen_fd);
    }
    return NULL;
}

int ezrtsp_start(  )
{
    int ret = -1;

    do  {
        /// start listen socket
        int rtsp_port = 554;
        int opt_reuse = 1;
        struct sockaddr_in serv_addr;

        memset( &serv_addr, 0, sizeof(struct sockaddr_in ) );

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl( INADDR_ANY );
        serv_addr.sin_port = htons( rtsp_port );

        g_listen_fd = socket( AF_INET, SOCK_STREAM, 0 );
        if( g_listen_fd <= 0 )  {
            err("socket open listen failed. [%d]\n", errno );
            break;
        }

        setsockopt( g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(int) );
        int opt_nbio = 1;
        ioctl( g_listen_fd, FIONBIO, &opt_nbio );

        if( 0 != bind( g_listen_fd, (struct sockaddr*)&serv_addr, sizeof(struct sockaddr) ) ) {
            err("socket bind failed. [%d] [%s]\n", errno, strerror(errno) );
            break;
        }

        if( 0 != listen( g_listen_fd, 10 ) ) {
            err("socket listen failed. [%d]\n", errno );
            break;
        }

        dbg("ezrtsp serv listen on port:[%d]\n", rtsp_port );
        ret = 0;
    } while(0);

    if( ret == -1 ) {
        err("ezrtsp start failed\n");
        if( g_listen_fd > 0 ) {
            close(g_listen_fd);
        }
        return -1;
    }

    /// start listen task 
    ez_rtsp_stat |= EZRTSP_INIT;
    if( 0 != pthread_create(&ez_rtsp_task_pid, NULL, &ezrtsp_task, NULL) ) {
        err("ezrtsp task create failed. [%d]\n", errno );
        return -1;
    }

    dbg("ezrtsp start finish\n");
    return 0;
}

int ezrtsp_stop( )
{
    if( ez_rtsp_stat & EZRTSP_INIT ) {
        ez_rtsp_stat &= ~EZRTSP_INIT;
        pthread_join( ez_rtsp_task_pid, NULL );
    }
    return 0;
}

