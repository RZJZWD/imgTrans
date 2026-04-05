// 系统函数
// #define _GNU_SOURCE
// #include <pthread.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h> // 提供 SYS_gettid
#include <sys/time.h>
#include <termios.h> // 终端属性控制
#include <time.h>
#include <unistd.h>
// 自封装函数
#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include "encode_to_video.h"
#include "img_transfer_config.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

#define INFO_LOG(user_msg, ...) printf(user_msg "\n", ##__VA_ARGS__)

/*******自定义变量***********/
// 运行标志位（静态全局，信号处理函数需要修改）
static volatile int keep_running = 1;
// 全局变量保存原始终端设置
static struct termios old_termios;
// 线程tid（仍为全局，方便主函数 join）
pthread_t camera_thread_id;
pthread_t display_thread_id;
pthread_t encode_thread_id;
pthread_t convert_decode_thread_id;
pthread_t out_h264_thread_id;
pthread_t out_mjpeg_thread_id;
pthread_t monitor_thread_id;

/********通用线程参数结构体**********/
typedef struct {
    EncoderContext *h264_ctx;     // H.264 编码器
    EncoderContext *mjpeg_ctx;    // MJPEG 编码器
    dmabuf_monitor_t *monitor;    // 监视器
    dmabuf_pool_t *pool;          // 缓冲池
    dmabuf_queue_t *camera_queue; // 摄像头原始数据队列
    dmabuf_queue_t *rgb_queue;    // RGB 数据队列
    dmabuf_queue_t *yuv_queue;    // 编码 专用队列
    int enable_display;           // 是否开启显示
    int width;                    // 图像宽度
    int height;                   // 图像高度
    int target_fps;               // 目标编码帧率
} thread_args_t;
// 协议兼容性条目
typedef struct {
    const char *protocol_prefix;   // 协议前缀，如 "rtmp://", "rtsp://",
                                   // "file://"（或普通文件）
    int supports_h264;             // 是否支持 H.264 编码
    int supports_mjpeg;            // 是否支持 MJPEG 编码
    const char *recommended_codec; // 推荐的编码器（用于提示）
} protocol_compatibility_t;
// 输出参数解析结果
typedef struct {
    const char *path;        // 输出路径（直接指向 argv，无需释放）
    enum AVCodecID codec_id; // 期望的编码器 ID
} output_arg_t;
/********自定义函数**********/
// 信号服务函数
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}
// 恢复终端设置的函数（程序退出时自动调用）
void restore_terminal(void) { tcsetattr(STDIN_FILENO, TCSANOW, &old_termios); }
static int64_t get_time_us(void) {
    struct timespec ts;
    if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts) == 0) {
        return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }
    // 如果 syscall 失败，回退到 gettimeofday
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    }
    return -1; // 错误
}

// ============================================================================
// 线程绑定核心表（基于 3 核 ARM A7，核心号 0、1、2）
// ----------------------------------------------------------------------------
// 当前绑核分配（线程函数内调用 bind_to_cpu）：
//   CPU 0: camera_thread_func, monitor_thread_func
//   CPU 1: convert_thread_func, display_thread_func
//   CPU 2: encode_thread_func, video_out_thread_func
// ============================================================================

static void bind_to_cpu(int cpu) {
    pid_t tid = syscall(SYS_gettid);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("sched_setaffinity");
    }
}
/**
 * @brief 根据输出类型创建编码器
 * @param width 宽度
 * @param height 高度
 * @param fps 帧率
 * @param thread 编码线程数
 * @param queue_size 内部队列大小
 * @param paths 输出路径数组
 * @param count 路径数量
 * @param codec_id 编码器ID (AV_CODEC_ID_H264 或 AV_CODEC_ID_MJPEG)
 * @return 成功返回编码器指针，失败返回 NULL
 */
static EncoderContext *create_encoder_for_targets(int width, int height,
                                                  int fps, int thread,
                                                  int queue_size, char *paths[],
                                                  int count,
                                                  enum AVCodecID codec_id) {
    if (count == 0)
        return NULL;
    EncoderContext *ctx = NULL;
    int ret =
        encoder_init(&ctx, width, height, fps, thread, queue_size, codec_id);
    if (ret < 0) {
        fprintf(stderr, "初始化编码器失败 (codec_id=%d)\n", codec_id);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        if (encoder_add_output(ctx, paths[i]) < 0) {
            fprintf(stderr, "添加输出目标失败: %s\n", paths[i]);
            encoder_close(ctx);
            return NULL;
        }
    }
    return ctx;
}
/**
 * @brief 检查指定输出路径是否支持给定的编码器
 * @param table       协议兼容性表（以 protocol_prefix == NULL 的条目结尾）
 * @param path        输出路径（如 "rtmp://server/live", "output.mp4"）
 * @param codec_id    期望的编码器（AV_CODEC_ID_H264 或 AV_CODEC_ID_MJPEG）
 * @param recommended 若不为
 * NULL，返回该协议推荐的编码器名称字符串（指向表中的静态字符串）
 * @return 1 支持，0 不支持
 */
int is_codec_supported_for_path(const protocol_compatibility_t *table,
                                const char *path, enum AVCodecID codec_id,
                                const char **recommended) {
    if (!path || !table)
        return 0;
    for (int i = 0; table[i].protocol_prefix != NULL; i++) {
        size_t prefix_len = strlen(table[i].protocol_prefix);
        if (strncmp(path, table[i].protocol_prefix, prefix_len) == 0) {
            if (recommended)
                *recommended = table[i].recommended_codec;
            if (codec_id == AV_CODEC_ID_H264)
                return table[i].supports_h264;
            if (codec_id == AV_CODEC_ID_MJPEG)
                return table[i].supports_mjpeg;
            return 0;
        }
    }
    // 无匹配协议前缀，视为普通文件（如 "output.mp4"）
    // 默认支持两种编码器
    if (codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_MJPEG)
        return 1;
    return 0;
}
/**
 * @brief 根据协议兼容性表过滤输出目标，分别提取 H.264 和 MJPEG 的有效路径
 * @param table            协议兼容性表
 * @param targets          原始输出目标数组
 * @param target_count     原始输出目标数量
 * @param h264_paths       输出 H.264 路径数组（调用者提供缓冲区）
 * @param h264_count       输出 H.264 路径数量
 * @param mjpeg_paths      输出 MJPEG 路径数组
 * @param mjpeg_count      输出 MJPEG 路径数量
 * @return 返回有效目标总数（被保留的数量）
 */
static int filter_output_targets(const protocol_compatibility_t *table,
                                 const output_arg_t *targets, int target_count,
                                 const char *h264_paths[], int *h264_count,
                                 const char *mjpeg_paths[], int *mjpeg_count) {
    int valid_total = 0;
    *h264_count = 0;
    *mjpeg_count = 0;

    for (int i = 0; i < target_count; i++) {
        const char *path = targets[i].path;
        enum AVCodecID codec_id = targets[i].codec_id;

        const char *recommended = NULL;
        if (!is_codec_supported_for_path(table, path, codec_id, &recommended)) {
            fprintf(stderr, "警告: 协议不支持 %s 编码，输出目标 '%s' 被忽略",
                    (codec_id == AV_CODEC_ID_H264) ? "H.264" : "MJPEG", path);
            if (recommended)
                fprintf(stderr, "（推荐使用 %s 编码）", recommended);
            fprintf(stderr, "\n");
            continue;
        }

        if (codec_id == AV_CODEC_ID_H264) {
            if (*h264_count < VIDEO_OUTPUT_TARGET) {
                h264_paths[*h264_count] = path;
                (*h264_count)++;
                valid_total++;
            } else {
                fprintf(stderr, "警告: H.264 输出目标过多，忽略 %s\n", path);
            }
        } else if (codec_id == AV_CODEC_ID_MJPEG) {
            if (*mjpeg_count < VIDEO_OUTPUT_TARGET) {
                mjpeg_paths[*mjpeg_count] = path;
                (*mjpeg_count)++;
                valid_total++;
            } else {
                fprintf(stderr, "警告: MJPEG 输出目标过多，忽略 %s\n", path);
            }
        }
    }
    return valid_total;
}
// 线程函数指针
void *camera_thread_func(void *arg) {
    bind_to_cpu(0);
    thread_args_t *args = (thread_args_t *)arg;

    struct timespec next_time;
    int64_t interval_ns = 1000000000LL / (args->target_fps); // 帧间隔纳秒
    // 获取起始时间
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &next_time);

    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 5;

    while (keep_running) {
        // 绝对时间睡眠

        while (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME,
                       &next_time, NULL) == -1 &&
               errno == EINTR) {
            // 被信号中断，继续等待
        }
        // 计算下一个绝对时间点
        next_time.tv_nsec += interval_ns;
        if (next_time.tv_nsec >= 1000000000) {
            next_time.tv_sec += 1;
            next_time.tv_nsec -= 1000000000;
        }

        dmabuf_buffer_t *new_buffer =
            dmabuf_buffer_alloc(args->pool, capture_uvc_get_v4l2buf_size());
        if (!new_buffer) {
            printf("分配新缓冲区失败");
            continue;
        }

        dmabuf_buffer_t *old_buffer = capture_uvc_captureImg(new_buffer);
        if (old_buffer) {
            // 已填充缓冲区已成功取出
            dmabuf_queue_enqueue(args->camera_queue, old_buffer);
            // 取消外部对已填充缓冲区的引用
            dmabuf_unref(old_buffer);
        } else {
            // 捕获失败
            consecutive_failures++;
            // 已填充缓冲区取出失败，内部将其重新入队。old_buffer==NULL
            // 将申请的new_buffer取消外部引用
            dmabuf_unref(new_buffer);
            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                printf("连续 %d 次捕获失败，尝试重新初始化摄像头",
                       consecutive_failures);
                enum capture_color color = capture_uvc_get_color();
                capture_uvc_clean(args->pool);
                capture_uvc_init(args->width, args->height, color, args->pool,
                                 CAMERA_INIT_FRAMES, args->target_fps);
                consecutive_failures = 0;
                // 重新初始化后需要重新计算 next_time 避免时间跳跃
                syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &next_time);
            }
        }
    }
    INFO_LOG("%s 关闭", __FUNCTION__);
    return NULL;
}

void *convert_thread_func(void *arg) {
    bind_to_cpu(1);
    thread_args_t *args = (thread_args_t *)arg;

    int target_fps = args->target_fps;
    int64_t frame_interval = 1000000 / target_fps; // 微秒
    int64_t last_encode_time = get_time_us();

    while (keep_running) {
        // 等待摄像头队列非空（阻塞直到有数据）
        if (dmabuf_queue_wait(args->camera_queue) != 0) {
            // 等待失败（如被信号中断），继续循环
            continue;
        }

        // 1. 取出原始数据
        dmabuf_buffer_t *raw_data = dmabuf_queue_dequeue(args->camera_queue);
        if (!raw_data) {
            // 异常：信号量唤醒但队列空，补偿信号量
            dmabuf_sem_post(&args->camera_queue->sem);
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
            // 将数据入队到 yuv 队列（如果存在）
            if ((args->h264_ctx || args->mjpeg_ctx) && args->yuv_queue) {
                // // === 帧率控制 ===
                // int64_t now = get_time_us(); // 获取当前微秒时间
                // if ((now - last_encode_time) >= frame_interval) {
                //     last_encode_time = now;
                //     // 达到目标帧间隔，入队当前原始帧

                // } else {
                //     // 未达到间隔，丢弃此帧（YUV 数据会被释放）
                //     // 注意：yuv420p_data 在最后统一释放，但这里需要先释放？
                //     //
                //     因为我们后面统一释放，所以无需额外操作，只需标记不入队。
                //     // 但为了更清晰，可以提前释放并跳过后续操作
                //     // 这里保持逻辑：不入队，但 yuv420p_data 仍需在末尾释放
                // }
                dmabuf_queue_enqueue(args->yuv_queue, yuv420p_data);

                // =================
            }
        } else {
            fprintf(stderr, "图像转换失败\n");
        }
        // 释放本线程对资源的引用
        dmabuf_unref(yuv420p_data);
        dmabuf_unref(raw_data);
    }
    INFO_LOG("%s 关闭", __FUNCTION__);
    return NULL;
}

void *display_thread_func(void *arg) {
    bind_to_cpu(1);
    thread_args_t *args = (thread_args_t *)arg;
    while (keep_running) {
        if (dmabuf_queue_wait(args->rgb_queue) != 0) {
            continue;
        }

        dmabuf_buffer_t *display_data = dmabuf_queue_dequeue(args->rgb_queue);
        if (!display_data) {
            // 异常：信号量唤醒但队列空，补偿信号量
            dmabuf_sem_post(&args->rgb_queue->sem);
            continue;
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
    INFO_LOG("%s 关闭", __FUNCTION__);
    return NULL;
}

void *encode_thread_func(void *arg) {
    bind_to_cpu(2);

    thread_args_t *args = (thread_args_t *)arg;

    while (keep_running) {
        // 1. 自动丢帧：检查队列长度，超过阈值则丢弃最旧的多帧
        int queue_len = dmabuf_queue_length(args->yuv_queue);
        if (queue_len >=
            YUV_QUEUE_SIZE -
                VIDEO_FULL_DROPPED) { // 例如 YUV_QUEUE_MAX_LEN = 10
            int drop_count = VIDEO_FULL_DROPPED;
            for (int i = 0; i < drop_count; i++) {
                // 等待信号量（如果队列非空则立即返回并减少信号量）
                if (dmabuf_queue_wait(args->yuv_queue) != 0) {
                    break; // 等待失败，退出丢帧循环
                }
                dmabuf_buffer_t *old = dmabuf_queue_dequeue(args->yuv_queue);
                if (old) {
                    dmabuf_unref(old);
                } else {
                    // 异常情况：队列空但信号量已减，补偿信号量
                    dmabuf_sem_post(&args->yuv_queue->sem);
                    break;
                }
            }
            // 丢弃后重新获取队列长度，可能仍高于阈值，但会在下一次循环继续处理
            // 注意：丢弃后队列长度减少，但信号量可能未同步？需要确保 dequeue
            // 正确减少了信号量 若信号量实现正确，dequeue
            // 会减少信号量计数，无需额外补偿

            continue; // 跳过本次编码，立即进入下一轮继续检查
        }

        // 等待队列非空
        if (dmabuf_queue_wait(args->yuv_queue) != 0) {
            continue;
        }

        // 从队列取一帧
        dmabuf_buffer_t *yuv_data = dmabuf_queue_dequeue(args->yuv_queue);
        if (!yuv_data) {
            dmabuf_sem_post(&args->yuv_queue->sem);
            continue;
        }

        // 编码
        if (args->h264_ctx) {
            int ret = encoder_frame(
                args->h264_ctx, dmabuf_get_data_ptr(yuv_data), yuv_data->size);
            if (ret < 0) {
                fprintf(stderr, "h264_encoder_frame failed\n");
            }
        } else if (args->mjpeg_ctx) {
            int ret = encoder_frame(
                args->mjpeg_ctx, dmabuf_get_data_ptr(yuv_data), yuv_data->size);
            if (ret < 0) {
                fprintf(stderr, "mjpeg_encoder_frame failed\n");
            }
        } else {
            fprintf(stderr, "encoder_frame failed, no encoder\n");
        }

        dmabuf_unref(yuv_data);
    }
    INFO_LOG("%s 关闭", __FUNCTION__);
    return NULL;
}

void *video_out_thread_func(void *arg) {
    bind_to_cpu(2);

    thread_args_t *args = (thread_args_t *)arg;
    EncoderContext *encode_ctx =
        args->h264_ctx ? args->h264_ctx : args->mjpeg_ctx;
    if (!encode_ctx)
        return NULL;

    while (keep_running) {
        // 等待编码线程发送信号（检查队列是否为空）
        pthread_mutex_lock(&encode_ctx->queue_lock);
        while (encoder_queue_empty(encode_ctx) &&
               !encode_ctx->encoding_finished) {
            pthread_cond_wait(&encode_ctx->queue_cond, &encode_ctx->queue_lock);
        }
        // 如果队列为空，说明是编码结束导致的唤醒，且没有剩余包，直接退出
        if (encoder_queue_empty(encode_ctx)) {
            encode_mutex_unlock(&encode_ctx->queue_lock);
            break; // 这里退出线程
        }
        pthread_mutex_unlock(&encode_ctx->queue_lock);

        int ret = encoder_output_packets(encode_ctx);
        if (ret < 0) {
            // 返回值为-1，出错
            fprintf(stderr, "output_encoder_frame failed\n");
            break;
        }
        // else if (ret == 1) {
        //     // 返回值为1,队列暂时未空，等待
        //     usleep(1000);
        //     continue;
        // }
    }
    INFO_LOG("%s 关闭", __FUNCTION__);
    return NULL;
}

void *monitor_thread_func(void *arg) {
    bind_to_cpu(0);

    thread_args_t *args = (thread_args_t *)arg;
    while (keep_running) {
        // 刷新监视器信息（从屏幕顶部开始覆盖）
        // 缓冲池信息
        dmabuf_monitor_refresh(args->monitor, 5);
        // 编码器信息
        if (args->h264_ctx)
            encoder_print_performance(args->h264_ctx);
        if (args->mjpeg_ctx)
            encoder_print_performance(args->mjpeg_ctx);

        // 检查并重连 H.264 编码器的 broken 目标
        if (args->h264_ctx)
            encoder_reconnect_broken(args->h264_ctx);
        // 检查并重连 MJPEG 编码器的 broken 目标
        if (args->mjpeg_ctx)
            encoder_reconnect_broken(args->mjpeg_ctx);

        sleep(1); // 每秒刷新一次
        // usleep(100000); // 100 ms
    }
    INFO_LOG("%s 关闭", __FUNCTION__);
    return NULL;
}

// 主函数，初始化资源，创建线程
int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 全局兼容性表（按匹配优先级排列，越长越具体应放在前面）
    const protocol_compatibility_t compat_table[] = {
        {"rtmp://", 1, 0, "h264"},  // RTMP 仅支持 H.264
        {"rtsp://", 1, 1, "mjpeg"}, // RTSP 两者都支持（默认MJPEG为兼容旧版）
        {"file://", 1, 1, NULL},    // 本地文件都支持
        {"/", 1, 1, NULL},          // 绝对路径文件（Linux）
        {"./", 1, 1, NULL},         // 相对路径文件
        {NULL, 0, 0, NULL}          // 结束标志
    };

    // ------------------- 新增：终端设置 -------------------
    // 1. 保存原始终端配置
    if (tcgetattr(STDIN_FILENO, &old_termios) != 0) {
        perror("tcgetattr failed");
        return -1;
    }
    // 2. 注册退出时的恢复函数
    atexit(restore_terminal);
    // 3. 设置非规范模式（关闭行缓冲+回显）
    struct termios new_termios = old_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO); // 关闭规范模式+回显
    new_termios.c_cc[VMIN] = 0;              // 最小读取字符数=0（立即返回）
    new_termios.c_cc[VTIME] = 0;             // 超时时间=0（无等待）
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) != 0) {
        perror("tcsetattr failed");
        return -1;
    }
    // ------------------------------------------------------

    // 默认参数值（局部变量）
    int camera_width = CAMERA_WIDTH;
    int camera_height = CAMERA_HEIGHT;
    int camera_fps = VIDEO_TARGET_FRAMERATE;
    enum capture_color camera_format = CAP_YUYV;
    int enable_display = LOCAL_DISPLAY;
    // 全部输出目标
    output_arg_t output_targets[VIDEO_OUTPUT_TARGET];
    int output_target_count = 0;
    char *default_output_file = "output.mp4"; // 输出目标为0时默认输出

    int opt;
    /* 选项字符串：
     *c: 表示视频相关 -c 需要参数 宽 高 目标帧率 摄像头采集格式（jpeg/yuyv)
     *s: 表示本地显示 -s 需要参数 0/1 关闭/开启本地显示
     *o: 表示输出目标 -o 需要参数 输出总个数 输出路径:编码器（可指定）
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
            int count = atoi(optarg);
            if (count <= 0) {
                fprintf(stderr, "错误: -o 后面的输出数量必须为正整数\n");
                exit(EXIT_FAILURE);
            }
            if (optind + count > argc) {
                fprintf(stderr,
                        "错误: -o 指定了 %d 个输出，但只提供了 %d 个路径\n",
                        count, argc - optind);
                exit(EXIT_FAILURE);
            }
            for (int i = 0; i < count; i++) {
                char *arg = argv[optind++];
                const char *path = arg;
                enum AVCodecID desired_codec = AV_CODEC_ID_NONE;

                // 查找 ?codec= 参数
                char *codec_param = strstr(arg, "?codec=");
                if (codec_param) {
                    *codec_param = '\0'; // 截断 URL，?codec= 之前的部分作为路径
                    path = arg;
                    char *codec_str = codec_param + 7; // 跳过 "?codec="
                    // 提取编码器名称（直到字符串结束或遇到
                    // '&'，但这里简单处理到结尾） 若还有额外参数可继续扩展
                    if (strncasecmp(codec_str, "h264", 4) == 0 &&
                        (codec_str[4] == '\0' || codec_str[4] == '&')) {
                        desired_codec = AV_CODEC_ID_H264;
                    } else if (strncasecmp(codec_str, "mjpeg", 5) == 0 &&
                               (codec_str[5] == '\0' || codec_str[5] == '&')) {
                        desired_codec = AV_CODEC_ID_MJPEG;
                    } else {
                        fprintf(stderr,
                                "警告: 不支持的编码器 '%s'，使用默认规则\n",
                                codec_str);
                    }
                }

                // 如果没有显式指定编码器，使用默认规则
                if (desired_codec == AV_CODEC_ID_NONE) {
                    if (strncmp(path, "rtsp://", 7) == 0)
                        desired_codec = AV_CODEC_ID_MJPEG;
                    else
                        desired_codec = AV_CODEC_ID_H264;
                }

                if (output_target_count < VIDEO_OUTPUT_TARGET) {
                    output_targets[output_target_count].path = path;
                    output_targets[output_target_count].codec_id =
                        desired_codec;
                    output_target_count++;
                } else {
                    fprintf(stderr, "警告: 输出目标过多，忽略 %s\n", path);
                }
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
    // 这里将文件，RTMP，不推mjpeg的RTSP 划分为使用h264编码
    // 将推mjpeg的RTSP划分为使用mjpeg编码，相当于直接传输mjpeg图片
    EncoderContext *h264_ctx = NULL;
    EncoderContext *mjpeg_ctx = NULL;
    // 缓冲池（局部）
    dmabuf_pool_t *pool = NULL;
    dmabuf_queue_t *camera_queue = NULL;
    dmabuf_queue_t *rgb_queue = NULL;
    dmabuf_queue_t *yuv_queue = NULL; // 编码 专用队列
    // 缓冲区监视器句柄（局部）
    dmabuf_monitor_t *monitor = NULL;

    // 过滤输出目标
    const char *h264_paths[VIDEO_OUTPUT_TARGET];
    int h264_count = 0;
    const char *mjpeg_paths[VIDEO_OUTPUT_TARGET];
    int mjpeg_count = 0;

    int valid_count = filter_output_targets(
        compat_table, output_targets, output_target_count, h264_paths,
        &h264_count, mjpeg_paths, &mjpeg_count);
    if (valid_count == 0 && output_target_count > 0) {
        fprintf(stderr, "错误: 所有输出目标均不兼容，程序退出\n");
        goto error;
    }
    // 创建编码器并添加输出
    if (h264_count > 0) {
        h264_ctx = create_encoder_for_targets(
            camera_width, camera_height, camera_fps, ENCODE_THREAD, 5,
            (char **)h264_paths, h264_count, AV_CODEC_ID_H264);
        if (!h264_ctx)
            goto error;
    }
    if (mjpeg_count > 0) {
        mjpeg_ctx = create_encoder_for_targets(
            camera_width, camera_height, camera_fps, ENCODE_THREAD, 5,
            (char **)mjpeg_paths, mjpeg_count, AV_CODEC_ID_MJPEG);
        if (!mjpeg_ctx)
            goto error;
    }
    // 如果没有有效输出目标，使用默认文件
    if (h264_count == 0 && mjpeg_count == 0) {
        const char *default_path = "output.mp4";
        if (is_codec_supported_for_path(compat_table, default_path,
                                        AV_CODEC_ID_H264, NULL)) {
            h264_ctx = create_encoder_for_targets(
                camera_width, camera_height, camera_fps, ENCODE_THREAD, 5,
                (char **)&default_path, 1, AV_CODEC_ID_H264);
            if (!h264_ctx)
                goto error;
        } else {
            fprintf(stderr, "错误: 默认输出文件不支持 H.264 编码\n");
            goto error;
        }
    }

    // 初始化缓冲池
    pool = dmabuf_pool_create(POOL_SIZE, "pool (thread)");
    if (!pool) {
        printf("缓冲池创建失败\n");
        goto error;
    }
    // 创建摄像头原始数据队列
    camera_queue = dmabuf_queue_create(CAMERA_QUEUE_SIZE, "CAMERA");
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
    // 创建 编码器 专用原始数据队列
    if (h264_ctx || mjpeg_ctx) {
        yuv_queue = dmabuf_queue_create(YUV_QUEUE_SIZE, "YUV");
        if (!yuv_queue) {
            printf("YUV H.264 队列创建失败\n");
            goto error;
        }
    } else {
        yuv_queue = NULL;
    }

    // 初始化缓冲池监视器（根据实际存在的队列构建数组）
    dmabuf_queue_t *queues[4];
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
    thread_args_t args = {
        .h264_ctx = h264_ctx,
        .mjpeg_ctx = mjpeg_ctx,
        .monitor = monitor,
        .pool = pool,
        .camera_queue = camera_queue,
        .rgb_queue = rgb_queue,
        .yuv_queue = yuv_queue,
        .enable_display = enable_display,
        .width = camera_width,
        .height = camera_height,
        .target_fps = camera_fps,
    };

    // 创建线程，统一传入 args 指针
    pthread_create(&camera_thread_id, NULL, camera_thread_func, &args);

    pthread_create(&convert_decode_thread_id, NULL, convert_thread_func, &args);

    // 创建编码线程
    if (h264_ctx || mjpeg_ctx) {
        pthread_create(&encode_thread_id, NULL, encode_thread_func, &args);
    }
    // H.264 输出线程
    thread_args_t args_h264 = args; // 复制结构
    args_h264.h264_ctx = h264_ctx;  // 明确指定
    args_h264.mjpeg_ctx = NULL;
    if (h264_ctx) {
        pthread_create(&out_h264_thread_id, NULL, video_out_thread_func,
                       &args_h264);
    }
    // MJPEG 输出线程
    thread_args_t args_mjpeg = args;
    args_mjpeg.mjpeg_ctx = mjpeg_ctx;
    args_mjpeg.h264_ctx = NULL;
    if (mjpeg_ctx) {
        pthread_create(&out_mjpeg_thread_id, NULL, video_out_thread_func,
                       &args_mjpeg);
    }
    // 仅在显示开启时创建显示线程
    if (enable_display) {
        pthread_create(&display_thread_id, NULL, display_thread_func, &args);
    }
    pthread_create(&monitor_thread_id, NULL, monitor_thread_func, &args);

    // 主线程运行
    while (keep_running) {
        char c = 0;
        // 非阻塞读取1个字符（立即返回）
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n > 0 && c == 'q') { // 检测到q键
            INFO_LOG("检测到q键，准备退出程序");
            keep_running = 0;
        }
        usleep(100000); // 100ms休眠，降低CPU占用
    }
    if (h264_ctx) {
        pthread_mutex_lock(&h264_ctx->queue_lock);
        pthread_cond_broadcast(&h264_ctx->queue_cond);
        pthread_mutex_unlock(&h264_ctx->queue_lock);
    }
    if (mjpeg_ctx) {
        pthread_mutex_lock(&mjpeg_ctx->queue_lock);
        pthread_cond_broadcast(&mjpeg_ctx->queue_cond);
        pthread_mutex_unlock(&mjpeg_ctx->queue_lock);
    }

    // 等待线程结束
    pthread_join(camera_thread_id, NULL);
    if (convert_decode_thread_id)
        pthread_join(convert_decode_thread_id, NULL);
    if (encode_thread_id)
        pthread_join(encode_thread_id, NULL);
    if (out_h264_thread_id)
        pthread_join(out_h264_thread_id, NULL);
    if (out_mjpeg_thread_id)
        pthread_join(out_mjpeg_thread_id, NULL);
    if (enable_display) {
        pthread_join(display_thread_id, NULL);
    }
    pthread_join(monitor_thread_id, NULL);

    // 清理资源（与 error 标签相同）
    capture_uvc_clean(pool);
    if (enable_display) {
        display_rgb_cleanup();
    }
    if (h264_ctx) {
        encoder_close(h264_ctx);
        INFO_LOG("关闭h264编码");
    }
    if (mjpeg_ctx) {
        encoder_close(mjpeg_ctx);
        INFO_LOG("关闭mjpeg编码");
    }
    dmabuf_pool_destroy(pool);
    INFO_LOG("关闭缓冲池");
    dmabuf_queue_destroy(camera_queue);
    INFO_LOG("关闭摄像头队列");
    if (rgb_queue) {
        dmabuf_queue_destroy(rgb_queue);
        INFO_LOG("关闭rgb队列");
    }
    if (yuv_queue) {
        dmabuf_queue_destroy(yuv_queue);
        INFO_LOG("关闭yuv队列");
    }
    dmabuf_monitor_destory(monitor);
    INFO_LOG("关闭监视器");

    return 0;

error:
    if (h264_ctx)
        encoder_close(h264_ctx);
    if (mjpeg_ctx)
        encoder_close(mjpeg_ctx);
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