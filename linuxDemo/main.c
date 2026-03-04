// 系统函数
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
// 自封装函数
#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include "encode_to_video.h"
#include "img_transfer_config.h"

/*******自定义宏*************/
#define CAMERA_WIDTH (640)     // 摄像头宽，也是后面所有图像数据的宽
#define CAMERA_HEIGHT (480)    // 摄像头高，也是后面所有图像数据的高
#define VIDEO_FULL_DROPPED (6) // 视频队列满时丢弃队列项
#if (DMABUF_ENABLE_MALLOC)

#define CAMERA_INIT_FRAMES (8) // 摄像头初始化内部帧个数
#define POOL_SIZE (40)         // 缓冲池大小
#define CAMERA_QUEUE_SIZE (8)  // 摄像头缓冲队列大小，生产原始图像JPEG
#define RGB_QUEUE_SIZE (6) // rgb数据队列大小，jpeg解码消费原始图像，生产rgb图像
#define YUV_QUEUE_SIZE (24) // yuv420队列大小，在jpeg解码时，rgb转yuv420p

#elif (DMABUF_ENABLE_DMABUF)

#define CAMERA_INIT_FRAMES (2) // 摄像头初始化内部帧个数
#define POOL_SIZE (6)          // 缓冲池大小
#define CAMERA_QUEUE_SIZE (2)  // 摄像头缓冲队列大小，生产原始图像JPEG
#define RGB_QUEUE_SIZE (2) // rgb数据队列大小，jpeg解码消费原始图像，生产rgb图像
#define YUV_QUEUE_SIZE (2) // yuv420队列大小，在jpeg解码时，rgb转yuv420p

#endif
/*******自定义变量***********/
// 运行标志位
static volatile int keep_running = 1;
// 缓冲池句柄
dmabuf_pool_t *pool = NULL;
dmabuf_queue_t *camera_queue = NULL;
dmabuf_queue_t *rgb_queue = NULL;
dmabuf_queue_t *yuv_queue = NULL;
// 缓冲区监视器句柄
dmabuf_monitor_t *monitor = NULL;

// 视频编码器句柄
EncoderContext *ctx = NULL;
// 视频输出文件流名称
const char *output_file = "output.mp4";
const char *output_url = "rtmp://192.168.1.10/live/livestream";

// 线程tid
pthread_t camera_thread_id;
pthread_t jpeg_decode_thread_id;
pthread_t display_thread_id;
pthread_t video_thread_id;
pthread_t monitor_thread_id;
/********自定义函数**********/
// 信号服务函数
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}
// 线程函数指针
void *camera_thread_func(void *arg) {

    while (keep_running) {
        dmabuf_buffer_t *new_buffer =
            dmabuf_buffer_alloc(pool, capture_uvc_get_v4l2buf_size());
        dmabuf_buffer_t *old_buffer = capture_uvc_captureImg(new_buffer);
        if (old_buffer) {
            // 已填充缓冲区已成功取出

            // 入队摄像头捕获队列，无论入队成功与否，都要取消当前线程的引用
            dmabuf_queue_enqueue(camera_queue, old_buffer);
            // 取消外部对已填充缓冲区的引用
            dmabuf_unref(old_buffer);
        } else {
            // 已填充缓冲区取出失败，内部将其重新入队。old_buffer==NULL
            // 将申请的new_buffer取消外部引用
            dmabuf_unref(new_buffer);
        }
        usleep(1000);
    }
    return NULL;
}

void *jpeg_decode_thread_func(void *arg) {
    while (keep_running) {
        // 1. 从捕获队列取出 jpeg 数据
        dmabuf_buffer_t *raw_data = dmabuf_queue_dequeue(camera_queue);
        if (!raw_data) {
            usleep(1000);
            continue;
        }

        // 2. 分配 YUV420P 缓冲区（编码用）
        dmabuf_buffer_t *yuv420p_data =
            dmabuf_buffer_alloc(pool, CAMERA_WIDTH * CAMERA_HEIGHT * 3 / 2);
        if (!yuv420p_data) {
            dmabuf_unref(raw_data);
            usleep(1000);
            continue;
        }
        // 3.将jpeg转为yuv420p数据
        if (jpeg_to_yuv420p_turbo(dmabuf_get_data_ptr(raw_data), raw_data->size,
                                  dmabuf_get_data_ptr(yuv420p_data),
                                  CAMERA_WIDTH, CAMERA_HEIGHT) == 0) {
            // 4.将yuv420p转为rgb
            dmabuf_buffer_t *rgb_data =
                dmabuf_buffer_alloc(pool, CAMERA_WIDTH * CAMERA_HEIGHT * 3);
            if (rgb_data) {
                // rgb_data分配成功，失败则跳过此帧
                if (yuv420p_to_rgb888_sw(dmabuf_get_data_ptr(yuv420p_data),
                                         dmabuf_get_data_ptr(rgb_data),
                                         CAMERA_WIDTH, CAMERA_HEIGHT) == 0) {
                    dmabuf_queue_enqueue(rgb_queue, rgb_data);
                }
                // 释放当前线程对 rgb 的引用（队列已持有）
                dmabuf_unref(rgb_data);
            }
            // 转换成功，将 YUV 入队供编码线程使用
            dmabuf_queue_enqueue(yuv_queue, yuv420p_data);
        }
        // 释放资源
        dmabuf_unref(yuv420p_data);
        dmabuf_unref(raw_data);
    }
    return NULL;
}
void *display_thread_func(void *arg) {
    while (keep_running) {
        dmabuf_buffer_t *display_data = dmabuf_queue_dequeue(rgb_queue);
        if (!display_data) {
            // 出队失败，队列为空，短暂休眠
            usleep(1000);
            continue;
        }
        int ret = display_rgb_from_buffer(dmabuf_get_data_ptr(display_data),
                                          CAMERA_WIDTH, CAMERA_HEIGHT);
        if (ret < 0) {
            // 显示获取数据失败，取消当前线程引用
            dmabuf_unref(display_data);
            continue;
        }
        display_rgb_run();
        // 显示后取消当前线程的引用
        dmabuf_unref(display_data);
    }
    return NULL;
}
void *video_thread_func(void *arg) {
    while (keep_running) {
        int queue_len = dmabuf_queue_length(yuv_queue);
        // 当队列长度满时，丢弃最旧的 DISCARD_COUNT 帧
        if (queue_len >= (YUV_QUEUE_SIZE - 4)) {
            int drop_count = VIDEO_FULL_DROPPED;
            // 确保丢弃数量不超过当前队列长度
            if (drop_count > queue_len)
                drop_count = queue_len;
            // fprintf(stderr, "队列积压 (%d)，丢弃 %d 帧\n", queue_len,
            //         drop_count);
            for (int i = 0; i < drop_count; i++) {
                dmabuf_buffer_t *old = dmabuf_queue_dequeue(yuv_queue);
                if (old)
                    dmabuf_unref(old);
            }
            // 丢弃后重新获取队列长度（可选）
            queue_len = dmabuf_queue_length(yuv_queue);
        }

        dmabuf_buffer_t *yuv_data = dmabuf_queue_dequeue(yuv_queue);
        if (!yuv_data) {
            usleep(1000);
            continue;
        }

        int ret =
            encode_frame(ctx, dmabuf_get_data_ptr(yuv_data), yuv_data->size);
        if (ret < 0) {
            fprintf(stderr, "encode_frame failed\n");
        }
        dmabuf_unref(yuv_data);
    }
    return NULL;
}
void *monitor_thread_func(void *arg) {
    while (keep_running) {
        dmabuf_monitor_refresh(monitor); // 刷新监视器信息（从屏幕顶部开始覆盖）
        sleep(1);                        // 每秒刷新一次
        // usleep(100000); // 100 ms
    }
    return NULL;
}
// 主函数，初始化资源，创建线程
int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    int ret = 0;

    // 初始化缓冲池
    pool = dmabuf_pool_create(POOL_SIZE, "pool (thread)");
    if (!pool) {
        printf("缓冲池创建失败\n");
        goto error;
    }
    camera_queue = dmabuf_queue_create(CAMERA_QUEUE_SIZE, "camera");
    if (!camera_queue) {
        printf("摄像头数据队列创建失败\n");
        goto error;
    }
    rgb_queue = dmabuf_queue_create(RGB_QUEUE_SIZE, "RGB");
    if (!rgb_queue) {
        printf("rgb数据队列创建失败\n");
        goto error;
    }
    yuv_queue = dmabuf_queue_create(YUV_QUEUE_SIZE, "YUV420P");
    if (!yuv_queue) {
        printf("yuv420p数据队列创建失败\n");
        goto error;
    }
    // 初始化缓冲池监视器
    dmabuf_queue_t *queues[3] = {camera_queue, rgb_queue, yuv_queue};
    monitor = dmabuf_monitor_create(pool, queues, 3);
    if (!monitor) {
        printf("监视器创建失败\n");
        goto error;
    }

    // 初始化摄像头
    ret = capture_uvc_init(CAMERA_WIDTH, CAMERA_HEIGHT, CAP_JPEG, pool,
                           CAMERA_INIT_FRAMES, 25);
    if (ret == -1) {
        printf("UVC摄像头初始化失败\n");
        goto error;
    }
    // 设置摄像头参数，开启自动曝光/动态帧率
    // capture_uvc_set_camera(true, 100, true);

    // 初始化编码器
    ret = encoder_init(&ctx, CAMERA_WIDTH, CAMERA_HEIGHT, 25, 2);
    if (ret == -1) {
        printf("初始化编码器失败\n");
        goto error;
    }
    // 添加编码器输出流
    encoder_add_output(ctx, output_url);
    jpeg_get_version();

    // 初始化显示
    ret = display_rgb_init();
    if (ret == -1) {
        printf("初始化显示系统失败\n");
        goto error;
    }

    // 创建线程
    pthread_create(&camera_thread_id, NULL, camera_thread_func, NULL);
    pthread_create(&jpeg_decode_thread_id, NULL, jpeg_decode_thread_func, NULL);
    pthread_create(&video_thread_id, NULL, video_thread_func, NULL);
    // struct sched_param param;
    // param.sched_priority = 1; // 可根据系统调整
    // pthread_setschedparam(video_thread_id, SCHED_RR, &param);

    pthread_create(&display_thread_id, NULL, display_thread_func, NULL);
    pthread_create(&monitor_thread_id, NULL, monitor_thread_func, NULL);

    while (keep_running) {
        sleep(1);
    }
    keep_running = 0;

    // 等待线程结束
    pthread_join(camera_thread_id, NULL);
    pthread_join(jpeg_decode_thread_id, NULL);
    pthread_join(video_thread_id, NULL);
    pthread_join(display_thread_id, NULL);
    pthread_join(monitor_thread_id, NULL);

    // 清理资源（与 error 标签相同）
    capture_uvc_clean(pool);
    display_rgb_cleanup();
    encoder_close(ctx);
    dmabuf_pool_destroy(pool);
    dmabuf_queue_destroy(camera_queue);
    dmabuf_queue_destroy(rgb_queue);
    dmabuf_queue_destroy(yuv_queue);
    dmabuf_monitor_destory(monitor);

    return 0;

error:
    if (pool)
        dmabuf_pool_destroy(pool);
    if (camera_queue)
        dmabuf_queue_destroy(camera_queue);
    if (rgb_queue)
        dmabuf_queue_destroy(rgb_queue);
    if (yuv_queue)
        dmabuf_queue_destroy(yuv_queue);
    if (monitor)
        dmabuf_monitor_destory(monitor);
    return -1;
}