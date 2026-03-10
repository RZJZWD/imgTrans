#include "encode_to_video.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h> // 用于 gettimeofday
#include <unistd.h>

static volatile int keep_running = 1;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}

int main(int argc, char *argv[]) {
    const char *output_file = "output.mp4";
    const char *output_url = "rtmp://192.168.1.4/live/livestream";
    EncoderContext *encoder_ctx = NULL;
    int width = 640;
    int height = 480;
    int fps = 15;
    int total_frames = 500; // 默认 10 秒 (25fps * 10)

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

    // 记录开始时间
    struct timeval start, end;
    gettimeofday(&start, NULL);

    // 初始化编码器
    if (encoder_init(&encoder_ctx, width, height, fps, 1) < 0) {
        fprintf(stderr, "初始化编码器失败\n");
        return 1;
    }
    encoder_add_output(encoder_ctx, output_url);
    printf("开始编码 %d (分辨率 %dx%d, 预设帧率 %d fps)...\n", total_frames,
           width, height, fps);

    int frame_count = 0;
    while (keep_running && frame_count < total_frames) {
        if (encoder_frame(encoder_ctx, NULL, 0) < 0) {

            fprintf(stderr, "编码帧 %d 失败\n", frame_count);
            break;
        }
        encoder_output_packets(encoder_ctx);
        frame_count++;
        if (frame_count % 25 == 0) {
            printf("已编码 %d 帧\n", frame_count);
        }
        if (frame_count == 100) {
            encoder_add_output(encoder_ctx, output_file);
        } else if (frame_count == 300) {
            encoder_remove_output(encoder_ctx, output_file);
        }
    }

    // 记录结束时间
    gettimeofday(&end, NULL);
    double elapsed =
        (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double actual_fps = frame_count / elapsed;

    printf("\n编码完成：共 %d 帧，耗时 %.2f 秒，实际帧率 %.2f fps\n",
           frame_count, elapsed, actual_fps);
    printf("预设帧率为 %d fps，%s\n", fps,
           actual_fps >= fps ? "已达到实时要求" : "未达到实时要求");

    encoder_close(encoder_ctx);
    return 0;
}