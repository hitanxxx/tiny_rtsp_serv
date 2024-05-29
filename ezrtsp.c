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
#include "HM2P_Common.h"
#include "HM2P_Network.h"
#include "HM2P_Video.h"
#include "ezcache.h"
#include "event.h"
#include "ezrtsp.h"


#define METHOD_OPTIONS        1
#define METHOD_DESCRIBE     2
#define METHOD_SETUP        3
#define METHOD_PLAY         4

#define EZRTSP_INIT 0x1
#define EZRTP_INIT  0x1


#define EZRTSP_PORT 554     ///default rtsp port 
#define EZRTSP_MSS 1430     ///conservative value


typedef struct rtsp_con rtsp_con_t;
typedef struct rtsp_session 
{
    int trackid;    //0:video 1:audio
    unsigned short seq;
    unsigned int ssrc;
    unsigned int ts;
    int send_err;

    rtsp_con_t * c;
} rtsp_session_t;

/// @brief rtsp connection info
struct rtsp_con
{
    int fd;
    ev_ctx_t * ctx;

    char client_ip[64];
    char buffer[4096];
    char * pos;
    char * last;
    char * start;
    char * end;

    char ferr:1;
    char fuse:1;
    char fplay:1;
    char fcomplete:1;
    char fovertcp:1;
    char faudioenb:1;
    int ichn;
    int icseq;
    int imethod;
    int itrack;

    rtsp_session_t  session_audio;
    rtsp_session_t  session_video;
};

static int g_listen_fd = 0;

static pthread_t ez_rtsp_task_pid;
static int ez_rtsp_stat = 0;

static pthread_t ez_rtp_task_pid;
static int ez_rtp_stat = 0;

static rtsp_con_t * con_arr = NULL;
static pthread_mutex_t g_ezrtsp_lock = PTHREAD_MUTEX_INITIALIZER;



/// data for storge video paramsets data: pps/vps/sps etc
static char* g_video_vps[2] = {NULL};
static int g_video_vpsn[2] = {0};
static char *g_video_sps[2] = {NULL};
static int g_video_spsn[2] = {0};
static char *g_video_pps[2] = {NULL};
static int g_video_ppsn[2] = {0};
static int g_ps_stat[2] = {0};


void ezrtsp_request( ev_ctx_t * ctx, int fd, void * user_data, int trigger_type );


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
#if(0)
    /// try get nal uint count int here
    if( chn == 0 ) {
        dbg("===\n");
        int naln = 0;
        int nalnp = 0;
        int i = 0;
        unsigned char * p = NULL;
        unsigned char * nals = NULL;
        unsigned char * nale = NULL;
        int nal_len = 0;
        
        for( i = 0; i < stream->len; i++) {
            p = stream->data + i;

            if( ((i + 3) < stream->len) && (*p == 0x00) && ( *(p+1) == 0x00) && ( *(p+2) == 0x01) ) {
                naln++;
                i+=3;
            }
            if( ((i + 4) < stream->len) && (*p == 0x00) && ( *(p+1) == 0x00) && ( *(p+2) == 0x00) && ( *(p+3) == 0x01) ) {
                naln++;
                i+=4;
            }

            if( nalnp != naln ) {
                if( !nals ) {
                    nals = stream->data + i;
                } else {
                    nale = stream->data + i;
                    
                    nal_len = nale - nals;
                    dbg("naluint len [%d]\n", nal_len );
                    if( video_get_enctype() && stream->frame_type == FRAME_TYPE_I ) {
                        int naltyp = (nals[0]&0b01111110)>>1;
                        if(naltyp == 32) {
                            dbg("vps\n");
                        } else if ( naltyp == 33 ) {
                            dbg("sps\n");
                        } else if ( naltyp == 34 ) {
                            dbg("pps\n");
                        }
                    }
                    nals = nale;
                }
                nalnp = naln;
            }
        }
        nale = stream->data + i;
        nal_len = nale - nals;
        dbg("naluint len [%d]\n", nal_len );
    }
#endif


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
                    /*
                        h.265 nalu header format 
                        +---------------+---------------+
                        |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7|
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |F|   Type    |  LayerId  | TID |
                        +-------------+-----------------+
                    */
                
                    /// find VPS, SPS, PPS info form H265 iframe 
                    int nalu_type = (nal_start[0]&0b01111110)>>1;
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
                    /*
                        h.264 nalu header format
                          +---------------+
                          |0|1|2|3|4|5|6|7|
                          +-+-+-+-+-+-+-+-+
                          |F|NRI|  Type   |
                          +---------------+
                    */
                    /// find SPS, PPS form H264 iframe                    
                    int nalu_type = nal_start[0]&0b00011111;
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


int ezrtp_packet_send( int fd, char * data, int datan )
{
    int sendn = 0;
    int tryn = 0;

    while(sendn<datan) {
        int rc = send(fd, data+sendn, datan-sendn, 0);
        if(rc<0) {
            if((errno==EAGAIN)||(errno == EWOULDBLOCK) ) {
                sys_msleep(5);
                tryn++;
                if(tryn>=7) return -1;///drop the packet if failure times more than 7
                continue;
            }
            err("ezrtp send err. [%d]\n", errno);
            return -1;
        }
        sendn += rc;
        tryn = 0;
    }
    return 0;
}

int ezrtp_packet_build( rtsp_session_t * session, int payload, const char * data, int datan, int marker )
{
    char rtp_packet[EZRTSP_MSS] = {0};
    
    /// RTP over tcp header (4 byte)
    if(1) {
        rtp_packet[0] = '$';              // 0x24
        if( session->trackid == 0 ) {
            rtp_packet[1] = 0x00;         // same as SETUP interleaved
        } else {
            rtp_packet[1] = 0x02;           
        }
        PUT_16( rtp_packet + 2, (12 + datan) );   // rtp data length
    }

    /*
        rtp header format
           0                   1                   2                   3
           0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
          |V=2|P|X|  CC   |M|     PT      |       sequence number         |
          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
          |                           timestamp                           |
          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
          |           synchronization source (SSRC) identifier            |
          +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
          |            contributing source (CSRC) identifiers             |
          |                             ....                              |
          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */
    /// RTP header (12 byte)
    char * rtp_hdr = rtp_packet + 4;
    rtp_hdr[0] = 0x2<<6;  /// fixed value
    rtp_hdr[1] = ( (marker&0x1)<<7) | (payload&0x7f); // fixed value
    PUT_16( rtp_hdr + 2, session->seq );  /// sequence
    PUT_32( rtp_hdr + 4, session->ts );   /// PTS
    PUT_32( rtp_hdr + 8, session->ssrc ); /// SSRC

    /// RTP payload 
    memcpy( (rtp_packet + 12 + 4), data, datan );
    /// send RTP over TCP
    if( 0 != ezrtp_packet_send( session->c->fd, rtp_packet, 4 + 12 + datan ) ) {
        return -1;
    }
    session->seq = (session->seq+1)%0xffff;
    return 0;
}

int ezrtp_fu_264( rtsp_session_t * session, int payload, char * nal, int naln, int mark )
{
    /*  FU-A type
         0                   1                   2                   3
         0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        | FU indicator  |   FU header   |                               |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
        |                                                               |
        |                         FU payload                            |
        |                                                               |
        |                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |                               :...OPTIONAL RTP padding        |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

        FU indicator
       +---------------+
       |0|1|2|3|4|5|6|7|
       +-+-+-+-+-+-+-+-+
       |F|NRI|  Type   |
       +---------------+

       FU header
       +---------------+
       |0|1|2|3|4|5|6|7|
       +-+-+-+-+-+-+-+-+
       |S|E|R|  Type   |
       +---------------+
    */

    int ret = 0;
    char * fua = NULL;
    char pkt[EZRTSP_MSS] = {0};
    int pktn = 0;

    pkt[0] = (nal[0]&0b11100000) | 28;    /// FU indicator
    pkt[1] = 0x80 | (nal[0]&0b00011111);   /// FU header

    /// skip nalu header (h264 1 byte)
    fua = nal+1;
    naln -= 1;

    int single_trans_maxn = EZRTSP_MSS;
    single_trans_maxn -= 12;
    if(session->c->fovertcp) {
        single_trans_maxn -= 4;
    }
    single_trans_maxn -= 2;

    while( naln > single_trans_maxn ) {
        
        pktn = single_trans_maxn;
        memcpy( &pkt[2], fua, pktn );
        if( ( ret = ezrtp_packet_build( session, payload, pkt, pktn+2, 0 ) ) < 0 ) {
            return ret;
        }

        fua += pktn;
        naln -= pktn;
        pkt[1] &= ~0x80; /// clear start flag
    }
    pkt[1]  |= 0x40;  /// set end flag
    memcpy( &pkt[2], fua, naln );
    return ezrtp_packet_build( session, payload, pkt, (naln+2), 1 );
}

int ezrtp_fu_265( rtsp_session_t * session, int payload, char * nal, int naln, int mark )
{
    char * fua = NULL;
    char pkt[EZRTSP_MSS] = {0};
    int pktn = 0;
    int ret = -1;

    /*
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |    PayloadHdr (Type=49)       |   FU header   | DONL (cond)   |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-|
       | DONL (cond)   |                                               |
       |-+-+-+-+-+-+-+-+                                               |
       |                         FU payload                            |
       |                                                               |
       |                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                               :...OPTIONAL RTP padding        |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

       PayloadHr == h265 nalu header
       +---------------+---------------+
       |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7|
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |F|   Type    |  LayerId  | TID |
       +-------------+-----------------+

       FU heahder
       +---------------+
       |0|1|2|3|4|5|6|7|
       +-+-+-+-+-+-+-+-+
       |S|E|  FuType   |
       +---------------+
    */

    pkt[0] = 49<<1;
    pkt[1] = 1;
    
    pkt[2] = 0x80|((nal[0]&0b01111110)>>1);

    /// skip 2 byte h265 nalu header 
    fua = nal + 2;
    naln -= 2;

    int single_trans_limitn = EZRTSP_MSS;
    single_trans_limitn -= 12;
    if(session->c->fovertcp) {
        single_trans_limitn -= 4;
    }
    single_trans_limitn -= 3;
    
    while( naln > single_trans_limitn ) {
        pktn = single_trans_limitn;
        
        memcpy( &pkt[3], fua, pktn );
        if( ( ret = ezrtp_packet_build( session, payload, pkt, pktn+3, 0 ) ) < 0 ) {
            return ret;
        }
        
        fua += pktn;
        naln -= pktn;

        pkt[2] &= ~0x80;   /// clear start flag
    }
    pkt[2] |= 0x40; /// set end flag
    memcpy( &pkt[3], fua, naln );
    return ezrtp_packet_build( session, payload, pkt, (naln+3), 1 );
}

int ezrtp_send_audio_frame( rtsp_session_t * session, char * frame, int framen )
{
    const char * aac = NULL;
    char pkt[EZRTSP_MSS] = {0};
    int pktn = 0;

    int ret = -1;

    ///skip ADTS header
    framen -= 7;
    pkt[0] = 0x0;
    pkt[1] = 0x10;
    pkt[2] = (framen&0x1fe0)>>5;
    pkt[3] = (framen&0x1f)<<3;
    ///skip nalu header
    aac = frame + 7;

    int single_trans_limitn = EZRTSP_MSS;
    single_trans_limitn -= 12;
    if(session->c->fovertcp) {
        single_trans_limitn -= 4;
    }
    single_trans_limitn -= 4;

    while( framen > single_trans_limitn ) {
        pktn = single_trans_limitn;
        memcpy(&pkt[4], aac, pktn);
        ret = ezrtp_packet_build( session, 98, pkt, pktn+4, 0 );
        if(ret < 0) {
            break;
        }
        aac += pktn;
        framen -= pktn;
    }
    memcpy( &pkt[4], aac, framen );
    return ezrtp_packet_build( session, 98, pkt, framen+4, 1 );
}

int ezrtp_send_video_frame( rtsp_session_t * session, char * frame_data, int frame_len )
{
    int ret = 0;
    const char * p = frame_data;
    const char * end = p + frame_len;
    const char * nal_start = NULL, *nal_end = NULL;

    /// 1. get nalu 
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
        
        
        /// 2. send nalu ( single nalu packet/fragmention uint)
        int single_trans_maxn = EZRTSP_MSS;
        single_trans_maxn -= 12;
        if( session->c->fovertcp) {
            single_trans_maxn -= 4;
        }
        
        if( nlen <= single_trans_maxn ) {
            if( video_get_enctype() == 0 ) {
                ret = ezrtp_packet_build( session, 96, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1 );
            } else {
                ret = ezrtp_packet_build( session, 97, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1 );
            }
        } else {
            if( video_get_enctype() == 0 ) {
                ret = ezrtp_fu_264( session, 96, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1 );
            } else {
                ret = ezrtp_fu_265( session, 97, (char*)nal_start, nlen, (end > nal_end) ? 0 : 1 );
            }
        }

        if( ret < 0 ) {
            break;
        }
        nal_start = nal_end;
    }
    return ret;
}



int ezrtsp_con_alloc( rtsp_con_t ** out )
{
    int i = 0;
    int full = 1;
    pthread_mutex_lock(&g_ezrtsp_lock);
    for(i=0; i<8; i++) {
        if(!con_arr[i].fuse) {
            full = 0;
            break;
        }
    }

    if(full) {
        err("ezrtsp con full\n");
        pthread_mutex_unlock(&g_ezrtsp_lock);
        return -1;
    } else {
        rtsp_con_t * c = &con_arr[i];
        c->fuse = 1;
        c->fd = 0;
        c->ctx = NULL;
        memset(c->client_ip, 0x0, sizeof(c->client_ip));
        memset(c->buffer, 0x0, sizeof(c->buffer));
        c->start = c->pos = c->last = c->buffer;
        c->end = c->start + sizeof(c->buffer);

        c->fplay = 0;
        c->ferr = 0;
        c->fcomplete = 0;
        c->fovertcp = 0;

        c->icseq = 0;
        c->imethod = 0;
        c->itrack = 0;
        c->ichn = 0;
        memset( &c->session_video, 0, sizeof(c->session_video));
        memset( &c->session_audio, 0, sizeof(c->session_audio));        

        *out=c;
        pthread_mutex_unlock(&g_ezrtsp_lock);
        return 0;
    }
}

void ezrtsp_con_free( rtsp_con_t * c )
{
    pthread_mutex_lock(&g_ezrtsp_lock);
    dbg("ezrtsp [%p:%d] free\n", c, c->fd );
    if(c) {
        if(c->fd>0) {
            ev_timer_del(c->ctx, c->fd);
            ev_opt(c->ctx, c->fd, NULL, NULL, EV_NONE);
            close(c->fd);
        }    
        memset(c, 0x0, sizeof(rtsp_con_t));
    }
    pthread_mutex_unlock(&g_ezrtsp_lock);
}

void ezrtsp_con_expire( ev_ctx_t * ctx, int fd, void * user_data )
{
    rtsp_con_t * con = (rtsp_con_t*)user_data;
    con->ferr = 1;
}

void ezrtsp_response_send( ev_ctx_t * ctx, int fd, void * user_data, int op )
{
    rtsp_con_t * c = (rtsp_con_t *)user_data;
    
    assert( c != NULL );
    if(c->ferr) return;
    
    while(c->last>c->pos) {
        int sendn = send(c->fd, c->pos, c->last-c->pos, 0);
        if(sendn<0) {
            if((errno==EAGAIN)||(errno == EWOULDBLOCK) ) {
                ev_timer_add(ctx, fd, c, ezrtsp_con_expire, 5000); 
                return;
            }
            err("ezrtsp [%p:%d] send rsp err. [%d]\n", c, c->fd, errno);
            if(!c->ferr) c->ferr=1;
            return;
        }
        c->pos += sendn;
    }
    ev_timer_del(ctx, fd);
    dbg("ezrtsp [%p:%d] rsp send:%s\n", c, c->fd, c->start);
    memset(c->buffer, 0, sizeof(c->buffer));
    c->start = c->pos = c->last = c->buffer;
    c->end = c->start + sizeof(c->buffer);

    c->fcomplete = 0;
    c->imethod = 0;
    c->icseq = 0;
    ev_opt(ctx, fd, (void*)c, ezrtsp_request, EV_R);
}


int ezrtsp_response_sdpinfo( int chn, char * str )
{
    int vpsn = 0, spsn = 0, ppsn = 4;
    char vps[128] = {0}, sps[512] = {0}, pps[128] = {0};
    char vpsb64[128] = {0}, spsb64[512] = {0}, ppsb64[128] = {0};

    if(video_get_enctype()==0) {    /// H264
        ezrtsp_video_paramset_get(chn, NULL, NULL, sps, &spsn, pps, &ppsn);
        int level = (sps[1]<<16) | (sps[2]<<8)|sps[3];
        sys_base64_encode(sps, spsn, spsb64, sizeof(spsb64));
        sys_base64_encode(pps, ppsn, ppsb64, sizeof(ppsb64));
        sprintf(str, "profile-level-id=%06X;sprop-parameter-sets=%s,%s", level, spsb64, ppsb64);
    } else {    /// H265 
        ezrtsp_video_paramset_get(chn, vps, &vpsn, sps, &spsn, pps, &ppsn);
        sys_base64_encode(vps, vpsn, vpsb64, sizeof(vpsb64));
        sys_base64_encode(pps, ppsn, ppsb64, sizeof(ppsb64));
        sys_base64_encode(sps, spsn, spsb64, sizeof(spsb64));
        sprintf(str, "sprop-vps=%s;sprop-sps=%s;sprop-pps=%s", vpsb64, spsb64, ppsb64);
    }
    return 0;
}


void ezrtsp_response( ev_ctx_t * ctx, int fd, void * user_data, int op )
{
    rtsp_con_t * c = (rtsp_con_t *)user_data;

    assert( c != NULL );
    if(c->ferr) return;
   
    char wireless_ip[64] = {0}; /// get device ipaddress
    net_ifname_ip_get(IFNAME_WLAN, wireless_ip);

    memset(c->buffer, 0, sizeof(c->buffer)); /// reset buffer
    c->start = c->pos = c->last = c->buffer;
    c->end = c->start + sizeof(c->buffer);
    
    if(c->imethod==METHOD_OPTIONS) {
        c->last += snprintf( c->buffer, sizeof(c->buffer),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Date: Thu, Jan 01 1970 00:00:00 GMT\r\n"
            "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n"
            "\r\n",
            c->icseq
        );
    } else if (c->imethod==METHOD_DESCRIBE) {
        char sdp[1<<10] = {0};  ///build sdp string
        ezrtsp_response_sdpinfo(c->ichn, sdp);
        
        char payload[4<<10] = {0};
        int payloadn = 0;
        if(video_get_enctype()== 0) {
            payloadn += snprintf( payload+payloadn, sizeof(payload)-1-payloadn,
                "v=0\r\n"
                "s=ipcamera\r\n"
                "t=0 0\r\n"
                "a=control:*\r\n"
                "a=range:npt=0-\r\n"
                "a=recvonly\r\n"
                "m=video 0 RTP/AVP 96\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "b=AS:1024\r\n"
                "a=rtpmap:96 H264/90000\r\n"
                "a=framerate: %d\r\n"
                "a=fmtp:96 packetization-mode=1;%s\r\n"
                "a=control:track=0\r\n",
                video_get_fps(c->ichn),
                sdp
            );
        } else {
            payloadn += snprintf( payload+payloadn, sizeof(payload)-1-payloadn,
                "v=0\r\n"
                "s=ipcamera\r\n"
                "t=0 0\r\n"
                "a=control:*\r\n"
                "a=range:npt=0-\r\n"
                "a=recvonly\r\n"
                "m=video 0 RTP/AVP 97\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "b=AS:5000\r\n"
                "a=rtpmap:97 H265/90000\r\n"
                "a=framerate: %d\r\n"
                "a=fmtp:97 packetization-mode=1;%s\r\n"
                "a=control:track=0\r\n",
                video_get_fps(c->ichn),
                sdp
            );
        }
#ifndef FAC     
        c->faudioenb = 1;  /// support audio data for rtsp stream 
        int aac_samplerate = 8000;
        unsigned short config = 0;
        unsigned auobjtype = 0x2;
        unsigned aufreqidx = 0xb;
        unsigned auchan = 0x1;
        if( aac_samplerate == 8000 ) {
            aufreqidx = 0xb;
        } else if ( aac_samplerate == 16000 ) {
            aufreqidx = 0x8;
        } else if ( aac_samplerate == 24000 ) {
            aufreqidx = 0x6;
        } else if ( aac_samplerate == 32000 ) {
            aufreqidx = 0x5;
        } else if ( aac_samplerate == 44100 ) {
            aufreqidx = 0x4;
        } else if ( aac_samplerate == 48000 ) {
            aufreqidx = 0x3;
        }
        config |= ((auobjtype<<11)&0xf800);
        config |= ((aufreqidx<<7)&0x780);
        config |= ((auchan<<3)&0x78);
        char aacinfo[256] = {0};
        memset(aacinfo, 0, sizeof(aacinfo));
        snprintf( aacinfo, sizeof(aacinfo)-1,
            "a=fmtp:98 profile-level-id=1;"
            "mode=AAC-hbr;sizelength=13,indexlength=3;"
            "indexdeltalength=3;config=%04x\r\n",
            config
        );
        ///add audio sdp string
        payloadn += snprintf( payload+payloadn, sizeof(payload)-1-payloadn,
            "m=audio 0 RTP/AVP 98\r\n"
            "c=IN IPV4 0.0.0.0\r\n"
            "b=AS:50\r\n"
            "a=rtpmap:98 mpeg4-generic/8000/1\r\n"
            "%s"
            "a=control:track=1\r\n"
            ,
            aacinfo
        );
#endif  
        /// build resp string 
        c->last += snprintf( c->buffer, sizeof(c->buffer)-1,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Date: Thu, Jan 01 1970 03:34:59 GMT\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            c->icseq,
            payloadn,
            payload
        );
    } else if (c->imethod==METHOD_SETUP) {
        if(!c->fovertcp) {  /// only support TCP type
            c->last += snprintf( c->buffer, sizeof(c->buffer),
                "RTSP/1.0 461 Unsupported transport\r\n"
                "CSeq: %d\r\n"
                "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
                "\r\n",
                c->icseq
            );
        } else {
            if(c->itrack==0) {  ///video session
                c->session_video.c = c;
                c->session_video.seq = 0;
                c->session_video.ssrc = 0x252525;
                c->session_video.ts = 0;
                c->session_video.trackid = 0;
                c->last += snprintf( c->buffer, sizeof(c->buffer),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=252525;mode=\"play\"\r\n"
                    "Session: F89623B6\r\n"
                    "\r\n"
                    ,
                    c->icseq
                );
            } else { ///audio session
                c->session_audio.c = c;
                c->session_audio.seq = 1;
                c->session_audio.ssrc = 0x303030;
                c->session_audio.ts = 0;
                c->session_audio.trackid = 1;
                c->last += snprintf( c->buffer, sizeof(c->buffer),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
                    "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;ssrc=303030;mode=\"play\"\r\n"
                    "Session: F89623B6\r\n"
                    "\r\n"
                    ,
                    c->icseq
                );
            }
        }
    } else if (c->imethod==METHOD_PLAY) {
        c->last += snprintf( c->buffer, sizeof(c->buffer),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Date: Thu, Jan 01 1970 02:56:13 GMT\r\n"
            "Session: F89623B6\r\n"
            "RTP-Info: url=rtsp://%s:554/%s/track=0;seq=0;rtptime=0,url=rtsp://%s:554/%s/track=1;seq=0;rtptime=0\r\n"
            "\r\n"
            ,
            c->icseq,
            wireless_ip,
            (c->ichn==0?"main_ch":"sub_ch"),
            wireless_ip,
            (c->ichn==0?"main_ch":"sub_ch")
        );
        int tcp_nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const void *) &tcp_nodelay, sizeof(int));

        c->fplay = 1;
        dbg("ezrtsp [%p:%d] play. ichn [%d] icseq [%d]\n", c, c->fd, c->ichn, c->icseq );
    }
    ev_opt(ctx, fd, (void*)c, ezrtsp_response_send, EV_W);
    ezrtsp_response_send(ctx, fd, c, EV_W);
}

void ezrtsp_request( ev_ctx_t * ctx, int fd, void * user_data,  int op )
{
    rtsp_con_t * c = (rtsp_con_t*)user_data;
    char * p = NULL;

    assert(EV_R==op);
    if(c->ferr) return;

    while(!c->fcomplete) {
        int recvd = recv(fd, c->last, c->end-c->last, 0);
        if(recvd<=0) {
            if(recvd==0) {
                err("ezrtsp [%p:%d] peer closed\n", c, c->fd);
                if(!c->ferr) c->ferr=1;
                return;
            } else {
                if((errno == EAGAIN)||(errno == EWOULDBLOCK)) {
                    if(c->fplay) {
                        dbg("ezrtsp [%p:%d] played. recvd [%d]\n", c, c->fd, c->last-c->pos );
                        memset( c->buffer, 0, sizeof(c->buffer) ); /// reset the meta
                        c->start = c->pos = c->last = c->buffer;
                        c->end = c->start + sizeof(c->buffer);
                        ev_timer_del(ctx, fd);
                    } else {
                        ev_timer_add(ctx, fd, c, ezrtsp_con_expire, 5000);
                    }
                } else {
                    err("ezrtsp [%p:%d] recv err. [%d]\n", c, c->fd, errno);
                    if(!c->ferr) c->ferr=1;
                }
                return;
            }
        }
        c->last += recvd;
        p = strstr(c->pos, "\r\n\r\n");
        if(p) c->fcomplete=1;
    }
    ev_timer_del(ctx, fd);
    dbg("ezrtsp request:%s\n", c->pos);
    
    p = strstr(c->pos, "CSeq:");  /// find Cseq 
    if(!p) {
        err("ezrtsp no 'Cseq'\n");
        if(!c->ferr) c->ferr=1;
        return;
    }
    c->icseq = strtol(p+strlen("CSeq:"), NULL, 10);
    
    if( strstr(c->pos, "OPTIONS ")!=NULL) {  /// find METHOD 
        c->imethod = METHOD_OPTIONS;
    } else if (strstr(c->pos, "DESCRIBE ")!=NULL) {
        c->imethod = METHOD_DESCRIBE;
        if(strstr(c->pos, "/main_ch")!=NULL) { ///get request chnnel
            c->ichn = 0;
        } else if (strstr(c->pos, "/sub_ch")!=NULL) {
            c->ichn = 1;
        } else {
            err("ezrtsp DESCRIBE ch not support. use def 'sub_ch'\n");
            c->ichn = 1;
        }
    } else if (strstr(c->pos, "SETUP ")!=NULL) {
        c->imethod = METHOD_SETUP;
        c->fovertcp = (NULL==strstr(c->pos, "TCP"))?0:1;
        c->itrack = 0; /// find track number 
        if(strstr(c->pos, "/track=0")!=NULL) {
            c->itrack = 0;
        } else if (strstr(c->pos, "/track=1")!=NULL) {
            c->itrack = 1;
        } else {
            err("ezrtsp SETUP track not support. use def 'track=1'\n");
            c->itrack = 1;
        }
    } else if (strstr(c->pos, "PLAY " )!=NULL) {
        c->imethod = METHOD_PLAY;
    } else if (strstr(c->pos, "TEARDOWN " )!=NULL) {
        dbg("ezrtsp TEARDOWN con set err flag\n");
        if(!c->ferr) c->ferr=1;    
        return;
    } else {
        err("ezrtsp method not support.\n");
        if(!c->ferr) c->ferr=1;
        return;
    }
    ev_opt(ctx, fd, (void*)c, ezrtsp_response, EV_W);
    ezrtsp_response(ctx, fd, c, EV_W);
}

void ezrtsp_accept( ev_ctx_t * ctx, int listen_fd, void * user_data, int op )
{
    int fd = 0;
    rtsp_con_t * c = NULL;
    assert( EV_R == op );

    for(;;) {
        socklen_t addrn = sizeof(struct sockaddr_in);
        struct sockaddr_in addr;
        memset( &addr, 0, addrn );

        fd = accept(listen_fd, (struct sockaddr*)&addr, &addrn);
        if(fd==-1) {
            if( errno == EWOULDBLOCK ||
                errno == EAGAIN ||
                errno == EINTR ||
                errno == EPROTO ||
                errno == ECONNABORTED
            ) {
                break;
            }
            err("rtsp accept failed. [%d]\n", errno );
            break;
        }
        int nbio = 1;
        ioctl( fd, FIONBIO, &nbio );
        
        if(0 != ezrtsp_con_alloc(&c)) {
            err("ezrtsp alloc con failed\n");
            close(fd);
            break;
        }
        c->fd = fd;
        c->ctx = ctx;
        strcpy( c->client_ip, inet_ntoa(addr.sin_addr) );
        dbg("ezrtsp accept connection. [%p:%d] form [%s]\n", c, c->fd, c->client_ip );
        
        ev_opt(ctx, fd, (void*)c, ezrtsp_request, EV_R);
        ezrtsp_request(ctx, fd, c, EV_R);
    }    
    return;
}


void * ezrtsp_task( void * para )
{
    ev_ctx_t * ctx = NULL;

    SET_THREAD_NAME("hm2p_ezrtsp");
    if(0!=ev_create(&ctx)) {
        err("ezrtsp evctx create err\n");
        return NULL;
    }
    ev_opt(ctx, g_listen_fd, NULL, ezrtsp_accept, EV_R);
    while(ez_rtsp_stat&EZRTSP_INIT) {
        ev_loop(ctx);
    }
    if(g_listen_fd>0)close(g_listen_fd);
    return NULL;
}

static void * ezrtp_task( )
{
    SET_THREAD_NAME("hm2p_ezrtp");

    rtsp_con_t * c = NULL;
    int liven = 0;
    long long chn_seq[2] = {-1};

    while(ez_rtp_stat&EZRTP_INIT) {
        liven = 0;
        for(int i=0; i<8; i++) {
            if(con_arr[i].ferr) {
                ezrtsp_con_free(&con_arr[i]);
            } else if(con_arr[i].fplay) {
                liven++;
            }
        }        
        if(!liven) {
            chn_seq[0]=chn_seq[1]=-1;
            sys_msleep(100);
            continue;
        }
        for(int ch=0; ch<2; ch++) {
            if(chn_seq[ch]==-1) chn_seq[ch]=ezcache_last_idr(ch);
            ezcache_frm_t * frm = ezcache_frm_get(ch, chn_seq[ch]);
            if (frm) {
                for(int i=0; i<8; i++) {
                    c = &con_arr[i];
                    if(c->ferr) {
                        ezrtsp_con_free(c);
                        continue;
                    }
                    if(c->fuse&&c->fplay&&(c->ichn==ch)) {
                        if(frm->typ==0) {
                            if(c->faudioenb) {
                                if(0!=ezrtp_send_audio_frame(&c->session_audio, (char*)frm->data, frm->datan)) {
                                    c->session_audio.send_err++;
                                    if(c->session_audio.send_err>=50) {
                                        err("ezrtsp [%p:%d] rtp audio frm send errn [%d]\n", c, c->fd, c->session_audio.send_err);
                                        if(!c->ferr) c->ferr=1;
                                    }
                                } else {
                                    c->session_audio.send_err=0;
                                }
                                /// aac_fps = 1s all sample / aac each frame sample = frame number in 1s (aac_fps = 8000/1024)
                                float aac_fps = 8000.0f/1024.0f;
                                c->session_audio.ts += 8000/aac_fps;
                            }
                        } else {
                            if(0!=ezrtp_send_video_frame(&c->session_video, (char*)frm->data, frm->datan)) {
                                c->session_video.send_err++;
                                if( c->session_video.send_err>=50) {
                                    err("ezrtsp [%p:%d] rtp video frm send errn [%d]\n", c, c->fd, c->session_video.send_err);
                                    if(!c->ferr) c->ferr=1;
                                }
                            } else {
                                c->session_video.send_err=0;
                            }
                            c->session_video.ts += 90000/video_get_fps(ch);
                        }
                    }
                }
                sys_free(frm);
                chn_seq[ch]++;
            } else {
                sys_msleep(5);
            }
        }
    }
    return NULL;
}


int ezrtsp_start(  )
{
    dbg("================= ezrtsp init\n");
    struct sockaddr_in addr;
    memset( &addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(EZRTSP_PORT);

    g_listen_fd = socket( AF_INET, SOCK_STREAM, 0 );
    if(g_listen_fd<=0) {
        err("ezrtsp listen socket open err. [%d]\n", errno);
        return -1;
    }
    int opt_reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(int));
    int opt_nbio = 1;
    ioctl(g_listen_fd, FIONBIO, &opt_nbio);

    if(0!=bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr))) {
        err("ezrtsp listen socket bind err. [%d]\n", errno);
        close(g_listen_fd);
        return -1;
    }
    if(0!=listen(g_listen_fd,10)) {
        err("ezrtsp listen socket listen err. [%d]\n", errno );
        close(g_listen_fd);
        return -1;
    }
    con_arr = sys_alloc(8*sizeof(rtsp_con_t));
    if(!con_arr) {
        err("ezrtsp cons alloc err, [%d]\n", errno );
        close(g_listen_fd);
        return -1;
    }
    
    ez_rtsp_stat |= EZRTSP_INIT;
    if(0 != pthread_create(&ez_rtsp_task_pid, NULL, &ezrtsp_task, NULL)) {
        err("ezrtsp rtsptask create err. [%d]\n", errno );
        return -1;
    }
    ez_rtp_stat |= EZRTP_INIT;
    if( 0 != pthread_create( &ez_rtp_task_pid, NULL, &ezrtp_task, NULL ) ) {
        err("ezrtsp rtptask create err. [%d]\n", errno );
        return -1;
    }
    dbg("ezrtsp listen on [%s:%d]\n", "0.0.0.0", EZRTSP_PORT);
    return 0;
}

int ezrtsp_stop( )
{
    if( con_arr ) {
        int i = 0;
        for( i = 0; i < 8; i ++ ) {
            ezrtsp_con_free(&con_arr[i]);
        }
        sys_free(con_arr);
        con_arr = NULL;
    }

    if( ez_rtp_stat == 1 ) {
        ez_rtp_stat = 0;
        pthread_join( ez_rtp_task_pid, NULL );    
    }

    if( ez_rtsp_stat & EZRTSP_INIT ) {
        ez_rtsp_stat &= ~EZRTSP_INIT;
        pthread_join( ez_rtsp_task_pid, NULL );
    }    
    return 0;
}

