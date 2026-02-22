#include "encode_to_video.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile int keep_running = 1;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}

int main(int argc, char *argv[]) {
    const char *output_file = "output.mp4";
    int width = 640;
    int height = 480;
    int fps = 25;
    int total_frames = 250; // 默认 10 秒 (25fps * 10)
    int use_internal = 1;   // 使用内部生成

    // 简单命令行解析
    if (argc >= 2)
        output_file = argv[1];
    if (argc >= 3)
        total_frames = atoi(argv[2]);
    if (argc >= 4)
        width = atoi(argv[3]);
    if (argc >= 5)
        height = atoi(argv[4]);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化编码器
    if (init_encoder(output_file, width, height, fps) < 0) {
        fprintf(stderr, "初始化编码器失败\n");
        return 1;
    }

    printf("开始编码 %d 帧到 %s ...\n", total_frames, output_file);

    int frame_count = 0;
    while (keep_running && frame_count < total_frames) {
        if (encode_frame(NULL, 0) < 0) {
            fprintf(stderr, "编码帧 %d 失败\n", frame_count);
            break;
        }
        frame_count++;
        // 可选的进度输出
        if (frame_count % 25 == 0) {
            printf("已编码 %d 帧\n", frame_count);
        }
    }

    printf("编码完成，共 %d 帧。正在关闭编码器...\n", frame_count);
    close_encoder();

    return 0;
}