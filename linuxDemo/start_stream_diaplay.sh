#!/bin/bash

# 用法：./start_stream.sh [服务器IP]
# 若不提供 IP，则提示输入

if [ -z "$1" ]; then
    read -p "请输入服务器IP地址: " SERVER_IP
else
    SERVER_IP=$1
fi

# 构造推流命令
CMD="./img_trans -c 640 480 25 yuyv -s 1 -o 1 rtsp://${SERVER_IP}:8554/live?codec=h264 -r http://${SERVER_IP}:5000?device_id=1"

echo "执行命令: $CMD"
$CMD