#!/bin/bash
# 采集15秒640x480 YUYV原始视频
# 用法: ./scripts/capture.sh

ROOT_DIR="/userdata/video_benchmark"
RAW_FILE="${ROOT_DIR}/raw.yuv"
DURATION=10
WIDTH=640
HEIGHT=480
FRAMERATE=25            # 根据摄像头实际支持设置
INPUT_DEV="/dev/video0" # 修改为你的摄像头设备

echo "开始采集原始 YUYV 视频..."
ffmpeg -f v4l2 \
       -input_format yuyv422 \
       -video_size ${WIDTH}x${HEIGHT} \
       -framerate ${FRAMERATE} \
       -i ${INPUT_DEV} \
       -t ${DURATION} \
       -c copy \
       -f rawvideo \
       ${RAW_FILE}

echo "采集完成: ${RAW_FILE}"
# 验证采集帧数（预期 DURATION * FRAMERATE ≈ 450 帧）
ffprobe -f rawvideo -video_size ${WIDTH}x${HEIGHT} -pixel_format yuyv422 -i ${RAW_FILE} \
        -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1