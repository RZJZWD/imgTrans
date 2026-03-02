#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define UVC_WIDTH 640
#define UVC_HEIGHT 480
#define UVC_FRAMES 2

#define POOL_SIZE 6          // 缓冲池容量
#define V4L2_QUEUE_SIZE 2    // v4l2 队列容量
#define CONVERT_QUEUE_SIZE 2 // 转换队列容量
#define TOTAL_FRAMES 200     // 总捕获帧数
static volatile int keep_running = 1;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}

static long long get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// 定义步骤枚举，方便统计
enum Step {
    STEP_ALLOC_NEW,
    STEP_CAPTURE,
    STEP_ENQUEUE_V4L2,
    STEP_UNREF_OLD,
    STEP_DEQUEUE_V4L2,
    STEP_ALLOC_RGB,
    STEP_JPEG2RGB,
    STEP_ENQUEUE_CONVERT,
    STEP_UNREF_RGB,
    STEP_UNREF_CAMERA,
    STEP_DEQUEUE_CONVERT,
    STEP_DISPLAY_FROM,
    STEP_DISPLAY_RUN,
    STEP_UNREF_DISPLAY,
    STEP_COUNT
};

const char *step_names[STEP_COUNT] = {
    "alloc new buffer",  "capture frame",         "enqueue v4l2_queue",
    "unref old_buffer",  "dequeue v4l2_queue",    "alloc rgb buffer",
    "jpeg_to_rgb",       "enqueue convert_queue", "unref rgb_data",
    "unref camera_data", "dequeue convert_queue", "display_rgb_from_buffer",
    "display_rgb_run",   "unref rgb_data_display"};

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    dmabuf_pool_t *v4l2_pool = dmabuf_pool_create(POOL_SIZE, "pool");
    dmabuf_queue_t *v4l2_queue =
        dmabuf_queue_create(V4L2_QUEUE_SIZE, "v4l2_raw_data_queue");
    dmabuf_queue_t *convert_queue =
        dmabuf_queue_create(CONVERT_QUEUE_SIZE, "converted_data_queue");
    int ret;
    ret = capture_uvc_init(UVC_WIDTH, UVC_HEIGHT, CAP_JPEG, v4l2_pool,
                           UVC_FRAMES, 25);
    if (ret != 0) {
        perror("UVC摄像头初始化失败\n");
        return -1;
    }
    printf("UVC摄像头初始化成功\n");
    capture_uvc_set_camera(true, 10, true);
    if (display_rgb_init() < 0) {
        printf("初始化显示系统失败\n");
        return -1;
    }
    printf("显示系统初始化成功\n");

    size_t v4l2_buffer_size = capture_uvc_get_v4l2buf_size();

    // 统计变量
    long long step_total[STEP_COUNT] = {0};
    int step_count[STEP_COUNT] = {0};
    long long loop_total = 0;
    int loop_count = 0;

    // 帧计数
    int frame_count = 0;

    while (keep_running && frame_count < TOTAL_FRAMES) {
        long long loop_start = get_time_ms();

        // 1. alloc new buffer
        long long start = get_time_ms();
        dmabuf_buffer_t *new_buffer =
            dmabuf_buffer_alloc(v4l2_pool, v4l2_buffer_size);
        long long end = get_time_ms();
        step_total[STEP_ALLOC_NEW] += end - start;
        step_count[STEP_ALLOC_NEW]++;
        if (!new_buffer)
            break;

        // 2. capture
        start = get_time_ms();
        dmabuf_buffer_t *old_buffer;
        old_buffer = capture_uvc_captureImg(new_buffer);

        end = get_time_ms();
        step_total[STEP_CAPTURE] += end - start;
        step_count[STEP_CAPTURE]++;
        if (!old_buffer) {
            dmabuf_unref(new_buffer);
            break;
        }

        // 3. enqueue v4l2
        start = get_time_ms();
        dmabuf_queue_enqueue(v4l2_queue, old_buffer);
        end = get_time_ms();
        step_total[STEP_ENQUEUE_V4L2] += end - start;
        step_count[STEP_ENQUEUE_V4L2]++;

        // 4. unref old
        start = get_time_ms();
        dmabuf_unref(old_buffer);
        end = get_time_ms();
        step_total[STEP_UNREF_OLD] += end - start;
        step_count[STEP_UNREF_OLD]++;

        // 5. dequeue v4l2
        start = get_time_ms();
        dmabuf_buffer_t *camera_data = dmabuf_queue_dequeue(v4l2_queue);
        end = get_time_ms();
        step_total[STEP_DEQUEUE_V4L2] += end - start;
        step_count[STEP_DEQUEUE_V4L2]++;
        if (!camera_data)
            break;

        // 6. alloc rgb
        start = get_time_ms();
        dmabuf_buffer_t *rgb_data =
            dmabuf_buffer_alloc(v4l2_pool, UVC_WIDTH * UVC_HEIGHT * 3);
        end = get_time_ms();
        step_total[STEP_ALLOC_RGB] += end - start;
        step_count[STEP_ALLOC_RGB]++;
        if (!rgb_data) {
            dmabuf_unref(camera_data);
            break;
        }

        // 7. jpeg to rgb
        start = get_time_ms();
        jpeg_to_rgb(dmabuf_get_data_ptr(camera_data), camera_data->size,
                    rgb_data->data, UVC_WIDTH, UVC_HEIGHT);
        end = get_time_ms();
        step_total[STEP_JPEG2RGB] += end - start;
        step_count[STEP_JPEG2RGB]++;

        // 8. enqueue convert
        start = get_time_ms();
        dmabuf_queue_enqueue(convert_queue, rgb_data);
        end = get_time_ms();
        step_total[STEP_ENQUEUE_CONVERT] += end - start;
        step_count[STEP_ENQUEUE_CONVERT]++;

        // 9. unref rgb
        start = get_time_ms();
        dmabuf_unref(rgb_data);
        end = get_time_ms();
        step_total[STEP_UNREF_RGB] += end - start;
        step_count[STEP_UNREF_RGB]++;

        // 10. unref camera
        start = get_time_ms();
        dmabuf_unref(camera_data);
        end = get_time_ms();
        step_total[STEP_UNREF_CAMERA] += end - start;
        step_count[STEP_UNREF_CAMERA]++;

        // 11. dequeue convert
        start = get_time_ms();
        dmabuf_buffer_t *rgb_data_display = dmabuf_queue_dequeue(convert_queue);
        end = get_time_ms();
        step_total[STEP_DEQUEUE_CONVERT] += end - start;
        step_count[STEP_DEQUEUE_CONVERT]++;
        if (!rgb_data_display)
            break;

        // 12. display from buffer
        start = get_time_ms();
        display_rgb_from_buffer(dmabuf_get_data_ptr(rgb_data_display),
                                UVC_WIDTH, UVC_HEIGHT);
        end = get_time_ms();
        step_total[STEP_DISPLAY_FROM] += end - start;
        step_count[STEP_DISPLAY_FROM]++;

        // 13. display run
        start = get_time_ms();
        display_rgb_run();
        end = get_time_ms();
        step_total[STEP_DISPLAY_RUN] += end - start;
        step_count[STEP_DISPLAY_RUN]++;

        // 14. unref display
        start = get_time_ms();
        dmabuf_unref(rgb_data_display);
        end = get_time_ms();
        step_total[STEP_UNREF_DISPLAY] += end - start;
        step_count[STEP_UNREF_DISPLAY]++;

        loop_total += get_time_ms() - loop_start;
        loop_count++;
        frame_count++;
    }

    printf("\n程序停止，已捕获 %d 帧，准备退出...\n", frame_count);
    sleep(2);
    printf("清除资源\n");

    capture_uvc_clean(v4l2_pool);
    display_rgb_cleanup();
    dmabuf_pool_destroy(v4l2_pool);
    dmabuf_queue_destroy(v4l2_queue);
    dmabuf_queue_destroy(convert_queue);

    printf("\n========== Configuration ==========\n");
    printf("Buffer allocation mode: %s\n",
#if (USE_MALLOC)
           "malloc (USERPTR)"
#else
           "dmabuf"
#endif
    );
    printf("Pool size: %d, V4L2 queue size: %d, Convert queue size: %d\n",
           POOL_SIZE, V4L2_QUEUE_SIZE, CONVERT_QUEUE_SIZE);
    printf("width: %d height:%d \n", UVC_WIDTH, UVC_HEIGHT);
    // printf("====================================\n");
    // 打印统计结果
    printf("\n========== Performance Statistics ==========\n");
    printf("Total frames: %d\n", frame_count);
    if (loop_count > 0) {
        printf("Average loop time: %.2f ms (%.2f fps)\n",
               (double)loop_total / loop_count,
               1000.0 / ((double)loop_total / loop_count));
    }
    printf("\nAverage step times (ms):\n");
    for (int i = 0; i < STEP_COUNT; i++) {
        if (step_count[i] > 0) {
            printf("  %-25s: %8.3f ms (count: %d)\n", step_names[i],
                   (double)step_total[i] / step_count[i], step_count[i]);
        } else {
            printf("  %-25s: N/A\n", step_names[i]);
        }
    }
    printf("============================================\n");

    return 0;
}