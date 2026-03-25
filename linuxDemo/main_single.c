#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include "encode_to_video.h"
#include "img_transfer_config.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static volatile int keep_running = 1;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM)
        keep_running = 0;
}

static int64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(int argc, char *argv[]) {
    // 默认参数（与 img_trans_config.h 一致）
    int width = CAMERA_WIDTH;
    int height = CAMERA_HEIGHT;
    int fps = VIDEO_TARGET_FRAMERATE;
    enum capture_color camera_format = CAP_JPEG;
    int enable_display = LOCAL_DISPLAY;
    char *output_paths[VIDEO_OUTPUT_TARGET];
    int output_count = 0;

    // 解析命令行参数（与 main.c 完全相同）
    int opt;
    while ((opt = getopt(argc, argv, "cs:o:")) != -1) {
        switch (opt) {
        case 'c':
            if (optind + 3 > argc) {
                fprintf(stderr,
                        "错误: -c 选项需要四个参数: width height fps format\n");
                exit(EXIT_FAILURE);
            }
            width = atoi(argv[optind]);
            height = atoi(argv[optind + 1]);
            fps = atoi(argv[optind + 2]);
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
                if (output_count < VIDEO_OUTPUT_TARGET) {
                    output_paths[output_count++] = argv[optind + i];
                } else {
                    fprintf(stderr, "警告: 输出目标过多，最多支持 %d 个\n",
                            VIDEO_OUTPUT_TARGET);
                    break;
                }
            }
            optind += count;
            break;
        }
        default:
            fprintf(stderr,
                    "用法: %s [-c width height fps format] [-s 0/1] -o count "
                    "path1 ...\n",
                    argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    // 如果没有任何输出目标，使用默认文件
    if (output_count == 0) {
        output_paths[0] = "output.mp4";
        output_count = 1;
        printf("未指定输出，使用默认文件: output.mp4\n");
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化缓冲池
    dmabuf_pool_t *pool = dmabuf_pool_create(POOL_SIZE, "single_pool");
    if (!pool) {
        fprintf(stderr, "创建缓冲池失败\n");
        return -1;
    }

    // 初始化摄像头
    if (capture_uvc_init(width, height, camera_format, pool, CAMERA_INIT_FRAMES,
                         fps) < 0) {
        fprintf(stderr, "摄像头初始化失败\n");
        dmabuf_pool_destroy(pool);
        return -1;
    }

    // 如果启用显示，初始化显示系统
    if (enable_display) {
        if (display_rgb_init() < 0) {
            fprintf(stderr, "初始化显示系统失败\n");
            capture_uvc_clean(pool);
            dmabuf_pool_destroy(pool);
            return -1;
        }
        printf("显示系统初始化成功\n");
    }

    // 初始化编码器（输出到所有目标）
    EncoderContext *enc_ctx = NULL;
    if (encoder_init(&enc_ctx, width, height, fps, 1, 5, AV_CODEC_ID_H264) <
        0) {
        fprintf(stderr, "编码器初始化失败\n");
        if (enable_display)
            display_rgb_cleanup();
        capture_uvc_clean(pool);
        dmabuf_pool_destroy(pool);
        return -1;
    }
    for (int i = 0; i < output_count; i++) {
        if (encoder_add_output(enc_ctx, output_paths[i]) < 0) {
            fprintf(stderr, "添加输出目标失败: %s\n", output_paths[i]);
            encoder_close(enc_ctx);
            if (enable_display)
                display_rgb_cleanup();
            capture_uvc_clean(pool);
            dmabuf_pool_destroy(pool);
            return -1;
        }
    }

    // 预分配 YUV420P 缓冲区
    int yuv_size = width * height * 3 / 2;
    uint8_t *yuv_buf = malloc(yuv_size);
    if (!yuv_buf) {
        fprintf(stderr, "分配 YUV 缓冲区失败\n");
        encoder_close(enc_ctx);
        if (enable_display)
            display_rgb_cleanup();
        capture_uvc_clean(pool);
        dmabuf_pool_destroy(pool);
        return -1;
    }

    // 预分配 RGB 缓冲区（仅当显示启用时）
    uint8_t *rgb_buf = NULL;
    if (enable_display) {
        rgb_buf = malloc(width * height * 3);
        if (!rgb_buf) {
            fprintf(stderr, "分配 RGB 缓冲区失败\n");
            free(yuv_buf);
            encoder_close(enc_ctx);
            display_rgb_cleanup();
            capture_uvc_clean(pool);
            dmabuf_pool_destroy(pool);
            return -1;
        }
    }

    // 性能统计
    int frame_count = 0;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    int64_t last_perf_print = get_time_ms();

    while (keep_running) {
        dmabuf_buffer_t *new_buf =
            dmabuf_buffer_alloc(pool, capture_uvc_get_v4l2buf_size());
        if (!new_buf) {
            fprintf(stderr, "申请缓冲区失败\n");
            break;
        }

        dmabuf_buffer_t *filled_buf = capture_uvc_captureImg(new_buf);
        if (!filled_buf) {
            dmabuf_unref(new_buf);
            usleep(1000);
            continue;
        }

        // 转换为 YUV420P
        int convert_ret = -1;
        if (camera_format == CAP_JPEG) {
            convert_ret =
                jpeg_to_yuv420p_turbo(dmabuf_get_data_ptr(filled_buf),
                                      filled_buf->size, yuv_buf, width, height);
        } else if (camera_format == CAP_YUYV) {
            convert_ret = yuyv422_to_yuv420p_neno(
                dmabuf_get_data_ptr(filled_buf), yuv_buf, width, height);
        } else {
            fprintf(stderr, "未知颜色格式\n");
            convert_ret = -1;
        }

        if (convert_ret == 0) {
            // 如果启用显示，将 YUV420P 转换为 RGB 并显示
            if (enable_display && rgb_buf) {
                yuv420p_to_bgr888_neno(yuv_buf, rgb_buf, width, height);
                display_rgb_from_buffer(rgb_buf, width, height);
                display_rgb_run(); // 刷新一次显示
            }

            // 编码
            int ret = encoder_frame(enc_ctx, yuv_buf, yuv_size);
            if (ret < 0) {
                fprintf(stderr, "编码帧 %d 失败\n", frame_count);
            }
            // 立即输出所有已编码的包
            int out_ret;
            do {
                out_ret = encoder_output_packets(enc_ctx);
            } while (out_ret == 0);
            if (out_ret < 0) {
                fprintf(stderr, "输出包失败\n");
            }
        } else {
            fprintf(stderr, "转换失败\n");
        }

        dmabuf_unref(filled_buf);
        frame_count++;

        // 每秒打印一次编码器性能
        int64_t now = get_time_ms();
        if (now - last_perf_print >= 1000) {
            encoder_print_performance(enc_ctx);
            last_perf_print = now;
        }
    }

    // 清理资源
    free(yuv_buf);
    if (rgb_buf)
        free(rgb_buf);
    encoder_close(enc_ctx);
    if (enable_display)
        display_rgb_cleanup();
    capture_uvc_clean(pool);
    dmabuf_pool_destroy(pool);
    return 0;
}