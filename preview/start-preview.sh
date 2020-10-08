#!/bin/bash

file=$1
fps=$2

if [ -z "$file" ]; then
  echo "File not informed. Usage: ./playvideo.sh \"<file>\" <fps"
  exit -1
fi

if [ -z "$fps" ]; then
  echo "FPS not informed. Usage: ./playvideo.sh \"<file>\" <fps>"
  exit -1
fi

metadata=($(./previewfeed metadata $file))
width=${metadata[0]}
height=${metadata[1]}

./previewfeed feed $file $fps | /opt/ffmpeg/bin/ffmpeg -s ${width}x${height} -r $fps -pix_fmt yuyv422 -f rawvideo -i - -pix_fmt yuv420p -s 320x180 -sws_flags neighbor -codec:v mpeg1video -strict experimental -vsync 0 -b:v 1000k -g 30 -bf 0 -f fifo -fifo_format mpegts -drop_pkts_on_overflow 1 -attempt_recovery 1 -recovery_wait_time 1 -queue_size 100 http://127.0.0.1:8081/preview_unit0
# ./previewfeed feed $file $fps | /opt/ffmpeg/bin/ffmpeg -s ${width}x${height} -r $fps -pix_fmt yuyv422 -f rawvideo -i - -pix_fmt yuv420p -s 320x180 -sws_flags neighbor -f sdl a 
