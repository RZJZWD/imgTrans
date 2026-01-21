#include "capture_uvc.h"
#include "display_rgb.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile int keep_running = 1;

#define CAPTURE_WIDTH 640
#define CAPTURE_HEIGHT 480

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n程序停止,准备退出...\n");
        keep_running = 0;
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== UVC摄像头实时显示程序 ===\n");
    printf("按 Ctrl+C 停止程序\n");

    // 1. 初始化显示系统
    DisplayLabelConfig config = {.show_info_label = 1,
                                 .show_exit_label = 1,
                                 .info_label_x = 10,
                                 .info_label_y = 10,
                                 .info_text = "UVC_capture\n640x480"};

    if (display_rgb_init(&config) < 0) {
        printf("初始化显示系统失败\n");
        return -1;
    }
    printf("显示系统初始化成功\n");

    // 2. 初始化摄像头系统
    if (capture_uvc_init(CAPTURE_WIDTH, CAPTURE_HEIGHT, CAP_JPEG) < 0) {
        printf("初始化摄像头系统失败\n");
        display_rgb_cleanup();
        return -1;
    }
    printf("摄像头系统初始化成功\n");

    // 3. 显示初始图像
    uint8_t *initial_buffer = capture_uvc_getRGBbuffer();
    if (initial_buffer) {
        display_rgb_from_buffer(initial_buffer, CAPTURE_WIDTH, CAPTURE_HEIGHT);
    } else {
        // 如果没有初始缓冲区，显示测试图像
        display_rgb_test_image(CAPTURE_WIDTH, CAPTURE_HEIGHT);
    }

    printf("开始实时捕获...\n");
    // 4. 主循环：捕获 -> 显示
    while (keep_running) {
        if (capture_uvc_captureImg() < 0) {
            printf("捕获失败，等待\n");
            usleep(100000);
            continue;
        }
        uint8_t *rgb_buffer = capture_uvc_getRGBbuffer();
        if (rgb_buffer) {
            display_rgb_from_buffer(rgb_buffer, CAPTURE_WIDTH, CAPTURE_HEIGHT);
        }

        // 处理显示事件
        display_rgb_run();

        // 小延迟以控制帧率
        usleep(30000); // ~33 FPS
    }

    // 5. 清理资源
    printf("正在清理资源...\n");
    display_rgb_cleanup();
    capture_uvc_clean();

    printf("程序结束\n");
    return 0;
}