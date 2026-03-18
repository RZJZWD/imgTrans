// 系统函数
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
// 自封装函数
#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include "encode_to_video.h"
#include "img_transfer_config.h"

/*******自定义变量***********/
// 运行标志位（静态全局，信号处理函数需要修改）
static volatile int keep_running = 1;

// 线程tid（仍为全局，方便主函数 join）
pthread_t camera_thread_id;
pthread_t jpeg_decode_thread_id;
pthread_t display_thread_id;
pthread_t video_encode_thread_id;
pthread_t video_out_thread_id;
pthread_t monitor_thread_id;

/********通用线程参数结构体**********/
typedef struct {
    EncoderContext *encode_ctx;   // 编码器上下文
    dmabuf_monitor_t *monitor;    // 监视器
    dmabuf_pool_t *pool;          // 缓冲池
    dmabuf_queue_t *camera_queue; // 摄像头原始数据队列
    dmabuf_queue_t *rgb_queue;    // RGB 数据队列
    dmabuf_queue_t *yuv_queue;    // YUV 数据队列
    int enable_display;           // 是否开启显示
    int width;                    // 图像宽度
    int height;                   // 图像高度
    int target_fps;               // 目标编码帧率
    volatile int *keep_running;   // 运行标志指针
} thread_args_t;

/********自定义函数**********/
// 信号服务函数
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}
static int64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
// 线程函数指针
void *camera_thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    while (*args->keep_running) {
        dmabuf_buffer_t *new_buffer =
            dmabuf_buffer_alloc(args->pool, capture_uvc_get_v4l2buf_size());
        dmabuf_buffer_t *old_buffer = capture_uvc_captureImg(new_buffer);
        if (old_buffer) {
            // 已填充缓冲区已成功取出
            dmabuf_queue_enqueue(args->camera_queue, old_buffer);
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
    thread_args_t *args = (thread_args_t *)arg;
    while (*args->keep_running) {
        // 1. 从捕获队列取出 jpeg 数据
        dmabuf_buffer_t *raw_data = dmabuf_queue_dequeue(args->camera_queue);
        if (!raw_data) {
            usleep(1000);
            continue;
        }

        // 2. 分配 YUV420P 缓冲区（编码用）
        dmabuf_buffer_t *yuv420p_data =
            dmabuf_buffer_alloc(args->pool, args->width * args->height * 3 / 2);
        if (!yuv420p_data) {
            dmabuf_unref(raw_data);
            usleep(1000);
            continue;
        }

        // 3.将原始数据转为yuv420p数据
        int convert_ret = -1;
        enum capture_color color = capture_uvc_get_color();
        if (color == CAP_JPEG) {
            convert_ret = jpeg_to_yuv420p_turbo(
                dmabuf_get_data_ptr(raw_data), raw_data->size,
                dmabuf_get_data_ptr(yuv420p_data), args->width, args->height);
        } else if (color == CAP_YUYV) {
            convert_ret = yuyv422_to_yuv420p_neno(
                dmabuf_get_data_ptr(raw_data),
                dmabuf_get_data_ptr(yuv420p_data), args->width, args->height);
        } else {
            fprintf(stderr, "未知颜色格式\n");
            convert_ret = -1;
        }

        // 4.将yuv420p转为rgb
        if (convert_ret == 0) {
            // 4. 如果开启了显示，则分配 RGB 缓冲区并转换
            if (args->enable_display) {
                dmabuf_buffer_t *rgb_data = dmabuf_buffer_alloc(
                    args->pool, args->width * args->height * 3);
                if (rgb_data) {
                    if (yuv420p_to_bgr888_neno(
                            dmabuf_get_data_ptr(yuv420p_data),
                            dmabuf_get_data_ptr(rgb_data), args->width,
                            args->height) == 0) {
                        dmabuf_queue_enqueue(args->rgb_queue, rgb_data);
                    }
                    dmabuf_unref(rgb_data);
                }
            }
            // 5. 无论显示是否开启，YUV 数据都要入队供编码线程使用
            dmabuf_queue_enqueue(args->yuv_queue, yuv420p_data);
        } else {
            fprintf(stderr, "图像转换失败\n");
        }
        // 释放资源
        dmabuf_unref(yuv420p_data);
        dmabuf_unref(raw_data);
    }
    return NULL;
}

void *display_thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    while (*args->keep_running) {
        dmabuf_buffer_t *display_data = dmabuf_queue_dequeue(args->rgb_queue);
        if (!display_data) {
            // 出队失败，队列为空，短暂休眠
            usleep(1000);
            continue;
        }
        int ret = display_rgb_from_buffer(dmabuf_get_data_ptr(display_data),
                                          args->width, args->height);
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

void *video_encode_thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    EncoderContext *encode_ctx = args->encode_ctx;

    int target_fps = args->target_fps;
    int64_t frame_interval = 1000000 / target_fps; // 微秒
    int64_t last_encode_time = 0;

    while (*args->keep_running) {
        int queue_len = dmabuf_queue_length(args->yuv_queue);
        // 当队列长度满时，丢弃最旧的 DISCARD_COUNT 帧
        if (queue_len >= (YUV_QUEUE_SIZE - 4)) {
            int drop_count = VIDEO_FULL_DROPPED;
            // 确保丢弃数量不超过当前队列长度
            if (drop_count > queue_len)
                drop_count = queue_len;
            // fprintf(stderr, "队列积压 (%d)，丢弃 %d 帧\n", queue_len,
            //         drop_count);
            for (int i = 0; i < drop_count; i++) {
                dmabuf_buffer_t *old = dmabuf_queue_dequeue(args->yuv_queue);
                if (old)
                    dmabuf_unref(old);
            }
            // 丢弃后重新获取队列长度（可选）
            queue_len = dmabuf_queue_length(args->yuv_queue);
        }

        dmabuf_buffer_t *yuv_data = dmabuf_queue_dequeue(args->yuv_queue);
        if (!yuv_data) {
            usleep(1000);
            continue;
        }

        // === 帧率控制 ===
        int64_t now = get_time_us(); // 获取当前微秒时间
        if (last_encode_time != 0 &&
            (now - last_encode_time) < frame_interval) {
            // 未达到目标帧间隔，丢弃当前帧
            dmabuf_unref(yuv_data);
            continue;
        }
        last_encode_time = now;
        // =================

        int ret = encoder_frame(encode_ctx, dmabuf_get_data_ptr(yuv_data),
                                yuv_data->size);
        if (ret < 0) {
            fprintf(stderr, "encoder_frame failed\n");
        }
        dmabuf_unref(yuv_data);
    }
    return NULL;
}

void *video_out_thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    EncoderContext *encode_ctx = args->encode_ctx;
    while (*args->keep_running) {
        // 等待编码线程发送信号（检查队列是否为空）
        // pthread_mutex_lock(&encode_ctx->queue_lock);
        // while (encoder_queue_empty(encode_ctx)) {
        //     pthread_cond_wait(&encode_ctx->queue_cond,
        //                       &encode_ctx->queue_lock);
        // }
        // if (!keep_running) {
        //     pthread_mutex_unlock(&encode_ctx->queue_lock);
        //     break;
        // }
        // pthread_mutex_unlock(&encode_ctx->queue_lock);

        int ret = encoder_output_packets(encode_ctx);
        if (ret < 0) {
            // 返回值为-1，出错
            fprintf(stderr, "output_encoder_frame failed\n");
            break;
        } else if (ret == 1) {
            // 返回值为1,队列暂时未空，等待
            usleep(1000);
            continue;
        }
    }
    return NULL;
}

void *monitor_thread_func(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    while (*args->keep_running) {
        // 刷新监视器信息（从屏幕顶部开始覆盖）
        dmabuf_monitor_refresh(args->monitor, 5);
        encoder_print_performance(args->encode_ctx); // 传入编码器上下文
        sleep(1);                                    // 每秒刷新一次
        // usleep(100000); // 100 ms
    }
    return NULL;
}

// 主函数，初始化资源，创建线程
int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 默认参数值（局部变量）
    int camera_width = CAMERA_WIDTH;
    int camera_height = CAMERA_HEIGHT;
    int camera_fps = VIDEO_TARGET_FRAMERATE;
    enum capture_color camera_format = CAP_YUYV;
    int enable_display = LOCAL_DISPLAY;
    char *output_paths[VIDEO_OUTPUT_TARGET];
    int output_count = 0;
    const char *default_output_file = "output.mp4"; // 输出目标为0时默认输出

    int opt;
    /* 选项字符串：
     *c: 表示视频相关 -c 需要参数 宽 高 目标帧率 摄像头采集格式（jpeg/yuyv)
     *s: 表示本地显示 -s 需要参数 0/1 关闭/开启本地显示
     *o: 表示输出目标 -o 需要参数 输出总个数 输出路径
     */
    while ((opt = getopt(argc, argv, "cs:o:")) != -1) {
        switch (opt) {
        case 'c': {
            // -c width height fps format
            if (optind + 3 > argc) {
                fprintf(stderr,
                        "错误: -c 选项需要四个参数: width height fps format\n");
                exit(EXIT_FAILURE);
            }
            camera_width = atoi(argv[optind]);
            camera_height = atoi(argv[optind + 1]);
            camera_fps = atoi(argv[optind + 2]);
            char *format_str = argv[optind + 3];
            if (strcasecmp(format_str, "jpeg") == 0) {
                camera_format = CAP_JPEG;
            } else if (strcasecmp(format_str, "yuyv") == 0) {
                camera_format = CAP_YUYV;
            } else {
                fprintf(stderr,
                        "错误: 不支持的格式 '%s'，仅支持 jpeg 或 yuyv\n",
                        format_str);
                exit(EXIT_FAILURE);
            }
            optind += 4;
            break;
        }
        case 's':
            enable_display = atoi(optarg);
            break;
        case 'o': {
            // -o count path1 path2 ...
            int count = atoi(optarg);
            if (count <= 0) {
                fprintf(stderr, "错误: -o 后面的输出数量必须为正整数\n");
                exit(EXIT_FAILURE);
            }
            // 检查剩余参数是否足够
            if (optind + count > argc) {
                fprintf(stderr,
                        "错误: -o 指定了 %d 个输出，但只提供了 %d 个路径\n",
                        count, argc - optind);
                exit(EXIT_FAILURE);
            }
            for (int i = 0; i < count; i++) {
                if (output_count >= VIDEO_OUTPUT_TARGET) {
                    fprintf(stderr,
                            "警告: 最多支持%d个输出路径，忽略多余的 '%s'\n",
                            VIDEO_OUTPUT_TARGET, argv[optind]);
                } else {
                    output_paths[output_count++] = argv[optind];
                }
                optind++;
            }
            break;
        }
        default:
            fprintf(stderr,
                    "用法: %s [-c width height fps format] [-s 0/1] -o count "
                    "path1 [path2 ...]\n",
                    argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    int ret = 0;
    // 视频编码器句柄（局部）
    EncoderContext *encode_ctx = NULL;
    // 缓冲区监视器句柄（局部）
    dmabuf_monitor_t *monitor = NULL;
    // 缓冲池（局部）
    dmabuf_pool_t *pool = NULL;
    dmabuf_queue_t *camera_queue = NULL;
    dmabuf_queue_t *rgb_queue = NULL;
    dmabuf_queue_t *yuv_queue = NULL;

    // 初始化缓冲池
    pool = dmabuf_pool_create(POOL_SIZE, "pool (thread)");
    if (!pool) {
        printf("缓冲池创建失败\n");
        goto error;
    }
    // 创建摄像头原始数据队列
    camera_queue = dmabuf_queue_create(CAMERA_QUEUE_SIZE, "camera");
    if (!camera_queue) {
        printf("摄像头数据队列创建失败\n");
        goto error;
    }
    // 创建显示所需的rgb数据队列
    if (enable_display) {
        // 根据显示开关决定是否创建 RGB 队列
        rgb_queue = dmabuf_queue_create(RGB_QUEUE_SIZE, "RGB");
        if (!rgb_queue) {
            printf("rgb数据队列创建失败\n");
            goto error;
        }
    } else {
        rgb_queue = NULL;
    }
    // 创建视频编码所需的yuv数据队列
    yuv_queue = dmabuf_queue_create(YUV_QUEUE_SIZE, "YUV420P");
    if (!yuv_queue) {
        printf("yuv420p数据队列创建失败\n");
        goto error;
    }

    // 初始化缓冲池监视器（根据实际存在的队列构建数组）
    dmabuf_queue_t *queues[3];
    int num_queues = 0;
    if (camera_queue)
        queues[num_queues++] = camera_queue;
    if (rgb_queue)
        queues[num_queues++] = rgb_queue;
    if (yuv_queue)
        queues[num_queues++] = yuv_queue;
    monitor = dmabuf_monitor_create(pool, queues, num_queues);
    if (!monitor) {
        printf("监视器创建失败\n");
        goto error;
    }

    // 初始化摄像头（使用解析得到的局部变量）
    ret = capture_uvc_init(camera_width, camera_height, camera_format, pool,
                           CAMERA_INIT_FRAMES, camera_fps);
    if (ret == -1) {
        printf("UVC摄像头初始化失败\n");
        goto error;
    }

    // 设置摄像头参数，开启自动曝光/动态帧率
    // capture_uvc_set_camera(true, 100, true);

    // 初始化编码器
    ret = encoder_init(&encode_ctx, camera_width, camera_height, camera_fps, 2,
                       5);
    if (ret == -1) {
        printf("初始化编码器失败\n");
        goto error;
    }
    // 添加输出目标

    if (output_count == 0) {
        // 如果没有指定输出目标，则输出默认目标
        encoder_add_output(encode_ctx, default_output_file);
    } else {
        for (int i = 0; i < output_count; i++) {
            encoder_add_output(encode_ctx, output_paths[i]);
        }
    }
    // 当使用jpeg时打印
    if (camera_format == CAP_JPEG) {
        jpeg_get_version();
    }

    // 如果开启显示，初始化显示系统
    if (enable_display) {
        ret = display_rgb_init();
        if (ret == -1) {
            printf("初始化显示系统失败\n");
            goto error;
        }
    }

    // 填充线程参数结构体
    thread_args_t args = {.encode_ctx = encode_ctx,
                          .monitor = monitor,
                          .pool = pool,
                          .camera_queue = camera_queue,
                          .rgb_queue = rgb_queue,
                          .yuv_queue = yuv_queue,
                          .enable_display = enable_display,
                          .width = camera_width,
                          .height = camera_height,
                          .target_fps = camera_fps,
                          .keep_running = &keep_running};

    // 创建线程，统一传入 args 指针
    pthread_create(&camera_thread_id, NULL, camera_thread_func, &args);
    pthread_create(&jpeg_decode_thread_id, NULL, jpeg_decode_thread_func,
                   &args);
    pthread_create(&video_encode_thread_id, NULL, video_encode_thread_func,
                   &args);
    pthread_create(&video_out_thread_id, NULL, video_out_thread_func, &args);
    // 仅在显示开启时创建显示线程
    if (enable_display) {
        pthread_create(&display_thread_id, NULL, display_thread_func, &args);
    }
    pthread_create(&monitor_thread_id, NULL, monitor_thread_func, &args);

    // 主线程运行
    while (keep_running) {
        sleep(1);
    }
    keep_running = 0;
    if (encode_ctx) {
        pthread_mutex_lock(&encode_ctx->queue_lock);
        pthread_cond_broadcast(&encode_ctx->queue_cond);
        pthread_mutex_unlock(&encode_ctx->queue_lock);
    }

    // 等待线程结束
    pthread_join(camera_thread_id, NULL);
    pthread_join(jpeg_decode_thread_id, NULL);
    pthread_join(video_encode_thread_id, NULL);
    pthread_join(video_out_thread_id, NULL);
    if (enable_display) {
        pthread_join(display_thread_id, NULL);
    }
    pthread_join(monitor_thread_id, NULL);

    // 清理资源（与 error 标签相同）
    capture_uvc_clean(pool);
    if (enable_display) {
        display_rgb_cleanup();
    }
    encoder_close(encode_ctx);
    dmabuf_pool_destroy(pool);
    dmabuf_queue_destroy(camera_queue);
    if (rgb_queue)
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