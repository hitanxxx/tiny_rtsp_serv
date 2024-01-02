# tiny_rtsp_serv
### a rtsp server based on linux platform not depend on any thrid party library (not complete yet)

### workflow
>>> parse H.264/H.265 video frame to NAL uint and storge into memory cache manager

>>> use RTSP protocol to control video stream

>>> use RTP protocol to send NAL uint on TCP connection (not support UDP yet)


### this project contains a real rtps server needs part. just like cache manager, video IDR frame pasr pps/vps/sps, RTP packet etc.it's understandable and simple
