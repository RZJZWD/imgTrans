// 系统函数
#include <pthread.h>
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
#define CAMERA_INIT_FRAMES (4) // 摄像头初始化内部帧个数

#define POOL_SIZE (10)        // 缓冲池大小
#define CAMERA_QUEUE_SIZE (4) // 摄像头缓冲队列大小，生产原始图像JPEG
#define RGB_QUEUE_SIZE (4) // rgb数据队列大小，jpeg解码消费原始图像，生产rgb图像

/*******自定义变量***********/
// 运行标志位
static volatile int keep_running = 1;
// 缓冲池句柄
dmabuf_pool_t *pool = NULL;
dmabuf_queue_t *camera_queue = NULL;
dmabuf_queue_t *rgb_queue = NULL;

// 缓冲区监视器句柄
dmabuf_monitor_t *monitor = NULL;

// 线程tid
pthread_t camera_thread_id;
pthread_t jpeg_decode_thread_id;
pthread_t display_thread_id;
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
    }
    return NULL;
}
void *jpeg_decode_thread_func(void *arg) {
    while (keep_running) {
        // 1. 从捕获队列取出原始数据
        dmabuf_buffer_t *raw_data = dmabuf_queue_dequeue(camera_queue);
        if (!raw_data) {
            // 队列为空，短暂休眠避免 CPU 空转
            usleep(1000);
            continue;
        }

        // 2. 申请存放 RGB 数据的缓冲区
        dmabuf_buffer_t *rgb_data =
            dmabuf_buffer_alloc(pool, CAMERA_WIDTH * CAMERA_HEIGHT * 3);
        if (!rgb_data) {
            // 分配失败，释放 raw_data 后重试
            dmabuf_unref(raw_data);
            usleep(1000); // 可选休眠，避免频繁重试
            continue;
        }

        // 3. 解码 JPEG 到 RGB
        jpeg_to_rgb(dmabuf_get_data_ptr(raw_data), raw_data->size,
                    dmabuf_get_data_ptr(rgb_data), CAMERA_WIDTH, CAMERA_HEIGHT);

        // 4. 将 RGB 数据入队，如果失败就会丢弃此rgb帧，无所谓
        dmabuf_queue_enqueue(rgb_queue, rgb_data);

        // 5. 释放当前线程对两个缓冲区的引用（队列已持有 rgb_data）
        dmabuf_unref(rgb_data);
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
void *monitor_thread_func(void *arg) {
    while (keep_running) {
        dmabuf_monitor_refresh(monitor); // 刷新监视器信息（从屏幕顶部开始覆盖）
        // sleep(1);                        // 每秒刷新一次
        usleep(100000); // 100 ms
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
        printf("摄像头数据队列创建失败\n");
        goto error;
    }
    // 初始化缓冲池监视器
    dmabuf_queue_t *queues[2] = {camera_queue, rgb_queue};
    monitor = dmabuf_monitor_create(pool, queues, 2);
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
    capture_uvc_set_camera(true, 100, true);

    // 初始化显示
    ret = display_rgb_init();
    if (ret == -1) {
        printf("初始化显示系统失败\n");
        goto error;
    }

    // 创建线程
    pthread_create(&camera_thread_id, NULL, camera_thread_func, NULL);
    pthread_create(&jpeg_decode_thread_id, NULL, jpeg_decode_thread_func, NULL);
    pthread_create(&display_thread_id, NULL, display_thread_func, NULL);
    pthread_create(&monitor_thread_id, NULL, monitor_thread_func, NULL);

    while (keep_running) {
        sleep(1);
    }
    keep_running = 0;

    // 等待线程结束
    pthread_join(camera_thread_id, NULL);
    pthread_join(jpeg_decode_thread_id, NULL);
    pthread_join(display_thread_id, NULL);
    pthread_join(monitor_thread_id, NULL);

    // 清理资源（与 error 标签相同）
    capture_uvc_clean(pool);
    display_rgb_cleanup();
    dmabuf_pool_destroy(pool);
    dmabuf_queue_destroy(camera_queue);
    dmabuf_queue_destroy(rgb_queue);
    dmabuf_monitor_destory(monitor);

    return 0;

error:
    if (pool)
        dmabuf_pool_destroy(pool);
    if (camera_queue)
        dmabuf_queue_destroy(camera_queue);
    if (rgb_queue)
        dmabuf_queue_destroy(rgb_queue);
    if (monitor)
        dmabuf_monitor_destory(monitor);
    return -1;
}