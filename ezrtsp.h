#ifndef __EZRTSP_H__
#define __EZRTSP_H__

#include "HM2P_Common.h"

int ezrtsp_start();
int ezrtsp_video_paramset_clear();
int ezrtsp_video_paramset_save(int chn, struct video_stream * stream);
int ezrtsp_audio_paramset_save(unsigned char * data);


#endif

