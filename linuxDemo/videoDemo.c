// 系统头文件放在最前面
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h> // 包含 gettimeofday
#include <time.h>
#include <unistd.h>

// 项目头文件放在后面
#include "capture_uvc.h"
#include "encode_to_video.h"

static volatile int keep_running = 1;
#define CAPTURE_WIDTH 640
#define CAPTURE_HEIGHT 480

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化摄像头
    printf("按 Ctrl+C 停止录像\n");
    printf("尝试初始化摄像头: %dx%d\n", CAPTURE_WIDTH, CAPTURE_HEIGHT);

    // 先尝试JPEG格式
    if (capture_uvc_init(CAPTURE_WIDTH, CAPTURE_HEIGHT, CAP_JPEG) < 0) {
        printf("JPEG格式初始化失败，尝试YUYV格式\n");
        // 如果JPEG失败，尝试YUYV
        if (capture_uvc_init(CAPTURE_WIDTH, CAPTURE_HEIGHT, CAP_YUYV) < 0) {
            printf("初始化摄像头系统失败\n");
            return -1;
        }
    }

    printf("摄像头系统初始化成功\n");

    // 初始化编码器
    if (init_encoder("uvc_video.mp4", CAPTURE_WIDTH, CAPTURE_HEIGHT, 25) < 0) {
        printf("初始化编码系统失败\n");
        capture_uvc_clean();
        return -1;
    }

    printf("开始实时录像...\n");

    int frame_count = 0;

    // 使用 gettimeofday 替代 clock
    struct timeval last_time, start_time, current_time;
    gettimeofday(&start_time, NULL);
    last_time = start_time;

    while (keep_running) {
        if (capture_uvc_captureImg() < 0) {
            printf("捕获失败，等待\n");
            sleep(1); // 减小等待时间
            continue;
        }

        int raw_buffer_size = 0;
        uint8_t *raw_buffer = capture_getRawbuffer(&raw_buffer_size);

        if (raw_buffer && raw_buffer_size > 100) { // 确保有足够的数据
            if (encode_frame(raw_buffer, raw_buffer_size) < 0) {
                printf("编码失败...\n");
                // 不break，继续尝试
            } else {
                frame_count++; // 只有编码成功时才计数
            }

            // 每100帧打印一次统计信息
            if (frame_count % 100 == 0) {
                gettimeofday(&current_time, NULL);

                // 计算时间差（秒）
                double elapsed =
                    (current_time.tv_sec - last_time.tv_sec) +
                    (current_time.tv_usec - last_time.tv_usec) / 1000000.0;

                if (elapsed > 0) {
                    double fps = 100.0 / elapsed;
                    printf("已编码 %d 帧, 当前FPS: %.2f\n", frame_count, fps);
                } else {
                    printf("已编码 %d 帧, 时间间隔太短\n", frame_count);
                }

                last_time = current_time;
            }
        } else {
            printf("获取原始数据失败或数据太小\n");
        }

        // // 控制帧率
        // usleep(30000); // 大约33fps
    }

    // 计算总时间
    gettimeofday(&current_time, NULL);
    double total_time = (current_time.tv_sec - start_time.tv_sec) +
                        (current_time.tv_usec - start_time.tv_usec) / 1000000.0;

    printf("\n程序停止，共编码 %d 帧，耗时 %.2f 秒，平均FPS: %.2f\n",
           frame_count, total_time,
           total_time > 0 ? frame_count / total_time : 0.0);

    printf("正在清理资源...\n");
    capture_uvc_clean();
    close_encoder();
    printf("程序结束\n");
    return 0;
}