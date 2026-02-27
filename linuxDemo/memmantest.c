#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#define UVC_WIDTH 640
#define UVC_HEIGHT 480
#define UVC_FRAMES 2

static volatile int keep_running = 1;
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}
int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    dmabuf_pool_t *v4l2_pool = dmabuf_pool_create(8);
    dmabuf_queue_t *v4l2_queue = dmabuf_queue_create(3);
    dmabuf_queue_t *convert_queue = dmabuf_queue_create(3);
    int ret = capture_uvc_init_dmabuf(UVC_WIDTH, UVC_HEIGHT, CAP_JPEG,
                                      v4l2_pool, UVC_FRAMES);
    if (ret != 0) {
        perror("UVC摄像头初始化失败\n");
        return -1;
    }
    printf("UVC摄像头初始化成功\n");

    // 1. 初始化显示系统
    if (display_rgb_init() < 0) {
        printf("初始化显示系统失败\n");
        return -1;
    }
    printf("显示系统初始化成功\n");

    size_t v4l2_buffer_size = capture_uvc_get_v4l2buf_size();
    while (keep_running) {
        dmabuf_buffer_t *new_buffer =
            dmabuf_buffer_alloc(v4l2_pool, v4l2_buffer_size);
        dmabuf_buffer_t *old_buffer = capture_uvc_captureImg_dmabuf(new_buffer);
        if (!old_buffer) {
            break;
        } else {
            dmabuf_queue_enqueue(v4l2_queue, old_buffer);
        }

        dmabuf_buffer_t *rgb_data =
            dmabuf_buffer_alloc(v4l2_pool, UVC_WIDTH * UVC_HEIGHT * 3);
        dmabuf_buffer_t *camera_data = dmabuf_queue_dequeue(v4l2_queue);
        dmabuf_ref(camera_data);
        dmabuf_ref(rgb_data);
        jpeg_to_rgb(camera_data->mmap_ptr, camera_data->size,
                    rgb_data->mmap_ptr, UVC_WIDTH, UVC_HEIGHT);
        dmabuf_queue_enqueue(convert_queue, rgb_data);
        dmabuf_unref(camera_data);
        dmabuf_unref(rgb_data);

        dmabuf_buffer_t *rgb_data_display = dmabuf_queue_dequeue(convert_queue);
        dmabuf_ref(rgb_data_display);
        display_rgb_from_buffer(rgb_data_display->mmap_ptr, UVC_WIDTH,
                                UVC_HEIGHT);
        dmabuf_unref(rgb_data_display);

        display_rgb_run();
    }
    printf("\n程序停止,准备退出...\n");
    sleep(2);
    printf("清除资源\n");
    capture_uvc_clean_dmabuf(v4l2_pool);
    display_rgb_cleanup();
    dmabuf_pool_destroy(v4l2_pool);
    dmabuf_queue_destroy(v4l2_queue);
    dmabuf_queue_destroy(convert_queue);
    return 0;
}