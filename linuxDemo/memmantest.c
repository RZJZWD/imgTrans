#include "capture_uvc.h"
#include "convert.h"
#include "display_rgb.h"
#include "dmabuf_manager.h"
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

struct run_time {
    long long start_time; /* 开始时间 (毫秒) */
    long long end_time;   /* 结束时间 (毫秒) */
    int run_cnt;          /* 实际运行次数 */
    const char *name;     /* 测试模块名称 */
};
static long long get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* 开始测量 */
void measure_run_start(struct run_time *module, int run_count,
                       const char *module_name) {
    module->start_time = get_time_ms();
    module->run_cnt = run_count;
    module->name = module_name;
    printf("[%s] 开始测试，计划运行 %d 次...\n", module_name, run_count);
}
/* 结束测量并打印统计 */
void measure_run_end(struct run_time *module) {
    module->end_time = get_time_ms();
    long long elapsed_ms = module->end_time - module->start_time;
    double avg_us = (elapsed_ms * 1000.0) / module->run_cnt;

    printf("[%s] 结束测试\n", module->name);
    printf("[%s] 总耗时: %lld ms, 运行次数: %d\n", module->name, elapsed_ms,
           module->run_cnt);
    printf("[%s] 平均耗时: %.2f us/次\n", module->name, avg_us);
    printf("----------------------------------------\n");
}

/* 便捷宏：自动测量一段代码块的执行时间（含预热） */
#define MEASURE_RUN(name, count, func)                                         \
    do {                                                                       \
        struct run_time __rt;                                                  \
        const int __warmup = 10; /* 预热次数，可根据需要调整 */                \
        /* 预热循环：不测量 */                                                 \
        for (int __i = 0; __i < __warmup; ++__i) {                             \
            (func);                                                            \
        }                                                                      \
        /* 正式计时循环 */                                                     \
        measure_run_start(&__rt, (count), (name));                             \
        for (int __i = 0; __i < (count); ++__i) {                              \
            (func);                                                            \
        }                                                                      \
        measure_run_end(&__rt);                                                \
    } while (0)

// 测试参数
#define TEST_WIDTH 640
#define TEST_HEIGHT 480
#define TEST_REPEAT 500 // 每个模式重复次数
int main(void) {
    // 根据编译配置确定默认转换模式
    const convert_mode_t convert_mode =
        (DMABUF_ALLOC_MODE == 0) ? CONVERT_MODE_CPU_SIMD : CONVERT_MODE_RGA;

    printf("=== 颜色转换性能测试 ===\n");
    printf("图像尺寸: %dx%d, 重复次数: %d\n", TEST_WIDTH, TEST_HEIGHT,
           TEST_REPEAT);
    printf("当前转换模式: %s\n",
           (convert_mode == CONVERT_MODE_RGA)        ? "RGA (硬件加速)"
           : (convert_mode == CONVERT_MODE_CPU_SIMD) ? "SIMD (NEON加速)"
                                                     : "CPU标量");

    // 1. 创建 dmabuf 池
    dmabuf_pool_t *pool = dmabuf_pool_create(4, "test_pool");
    if (!pool) {
        fprintf(stderr, "创建 dmabuf 池失败\n");
        return -1;
    }

    // 2. 分配缓冲区
    size_t yuyv_size = TEST_WIDTH * TEST_HEIGHT * 2;
    size_t yuv420_size = TEST_WIDTH * TEST_HEIGHT * 3 / 2;
    size_t rgb_size = TEST_WIDTH * TEST_HEIGHT * 3;

    dmabuf_buffer_t *src_yuyv = dmabuf_buffer_alloc(pool, yuyv_size);
    dmabuf_buffer_t *dst_yuv420 = dmabuf_buffer_alloc(pool, yuv420_size);
    dmabuf_buffer_t *dst_rgb = dmabuf_buffer_alloc(pool, rgb_size);

    if (!src_yuyv || !dst_yuv420 || !dst_rgb) {
        fprintf(stderr, "分配缓冲区失败\n");
        dmabuf_pool_destroy(pool);
        return -1;
    }

    // 3. 填充源 YUYV 测试数据
    uint8_t *src_ptr = dmabuf_get_data_ptr(src_yuyv);
    for (int i = 0; i < TEST_HEIGHT; i++) {
        for (int j = 0; j < TEST_WIDTH; j += 2) {
            int idx = i * TEST_WIDTH * 2 + j * 2;
            src_ptr[idx + 0] = (i + j) % 256;     // Y0
            src_ptr[idx + 1] = 128;               // U
            src_ptr[idx + 2] = (i + j + 1) % 256; // Y1
            src_ptr[idx + 3] = 128;               // V
        }
    }

    // 4. 执行三项核心转换测试
    // 转换1: YUYV422 -> YUV420P
    MEASURE_RUN("YUYV422 -> YUV420P", TEST_REPEAT,
                yuyv422_to_yuv420p(src_yuyv, dst_yuv420, TEST_WIDTH,
                                   TEST_HEIGHT, convert_mode));

    // 转换2: YUV420P -> RGB888 (需要上一步的输出作为输入)
    MEASURE_RUN("YUV420P -> RGB888", TEST_REPEAT,
                yuv420p_to_rgb888(dst_yuv420, dst_rgb, TEST_WIDTH, TEST_HEIGHT,
                                  convert_mode));
    // 如果当前是 SIMD 模式，额外追加标量 CPU 测试作为性能基准
    if (convert_mode == CONVERT_MODE_CPU_SIMD) {
        printf("\n>>> 追加标量 CPU 实现测试 (性能基准) <<<\n");

        // 注意：源数据 src_yuyv 未被修改，可直接复用
        MEASURE_RUN("YUYV422 -> YUV420P (CPU_SCALAR)", TEST_REPEAT,
                    yuyv422_to_yuv420p(src_yuyv, dst_yuv420, TEST_WIDTH,
                                       TEST_HEIGHT, CONVERT_MODE_CPU_SCALAR));

        MEASURE_RUN("YUV420P -> RGB888 (CPU_SCALAR)", TEST_REPEAT,
                    yuv420p_to_rgb888(dst_yuv420, dst_rgb, TEST_WIDTH,
                                      TEST_HEIGHT, CONVERT_MODE_CPU_SCALAR));
    }

    // 4. 清理资源
    dmabuf_buffer_free(pool, src_yuyv);
    dmabuf_buffer_free(pool, dst_yuv420);
    dmabuf_buffer_free(pool, dst_rgb);
    dmabuf_pool_destroy(pool);

    printf("\n测试完成。\n");
    return 0;
}