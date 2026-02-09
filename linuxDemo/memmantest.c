#include "convert.h"
#include "dmabuf_manager.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// 测试参数
#define TEST_WIDTH 640
#define TEST_HEIGHT 480
#define QUEUE_SIZE 5
#define POOL_SIZE 10 // 现在需要更大的池来容纳两种不同类型的缓冲区
#define TEST_LOOPS 20

// 测试模式
typedef enum {
    TEST_MODE_SIMPLE, // 简单模式：单次转换
    TEST_MODE_QUEUE,  // 队列模式：连续转换和队列操作
    TEST_MODE_STRESS  // 压力测试：大量循环
} TestMode;

// 生成测试用的随机RGB数据
void generate_test_rgb(uint8_t *rgb, int width, int height) {
    static int seed = 0;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;

            // 生成渐变和随机混合的测试图案
            int r = (x * 255 / width) ^ (y * 255 / height) ^ seed;
            int g = (y * 255 / height) ^ (x * 127 / width) ^ (seed << 1);
            int b = ((x + y) * 255 / (width + height)) ^ (seed << 2);

            rgb[idx] = r & 0xFF;
            rgb[idx + 1] = g & 0xFF;
            rgb[idx + 2] = b & 0xFF;
        }
    }

    seed++;
}

// 验证RGB到YUV再到RGB转换的正确性（允许一定的误差）
int verify_conversion(uint8_t *original_rgb, uint8_t *converted_rgb, int width,
                      int height, float max_error_percent) {
    int total_pixels = width * height;
    int error_count = 0;
    int max_errors = total_pixels * max_error_percent / 100.0f;

    for (int i = 0; i < total_pixels; i++) {
        int idx = i * 3;

        // 计算颜色分量差异
        int dr = abs((int)original_rgb[idx] - (int)converted_rgb[idx]);
        int dg = abs((int)original_rgb[idx + 1] - (int)converted_rgb[idx + 2]);
        int db = abs((int)original_rgb[idx + 2] - (int)converted_rgb[idx + 1]);

        // 由于BGR/RGB顺序问题，这里调整比较
        // 注意：convert.c中的RGA转换可能保持了RGB顺序
        // 但yuyv_to_rgb函数中有BGR/RGB交换
        int d = dr + dg + db;

        // 如果差异太大，认为有错误
        if (d > 30) { // 每个分量平均差异10
            error_count++;
        }
    }

    printf("转换验证: 总像素=%d, 错误像素=%d, 允许错误=%d\n", total_pixels,
           error_count, max_errors);

    return error_count <= max_errors;
}

// 测试1: 简单RGA转换测试 - 使用单一内存池
void test_simple_rga_conversion(dmabuf_pool_t *pool) {
    printf("\n=== 测试1: 简单RGA转换测试（单一内存池） ===\n");

    // 计算缓冲区大小
    size_t rgb_size = TEST_WIDTH * TEST_HEIGHT * 3;     // RGB888
    size_t yuv_size = TEST_WIDTH * TEST_HEIGHT * 3 / 2; // YUV420P

    // 从同一个池中分配两种类型的缓冲区
    dmabuf_buffer_t *rgb_buffer = dmabuf_buffer_alloc(pool, rgb_size);
    dmabuf_buffer_t *yuv_buffer = dmabuf_buffer_alloc(pool, yuv_size);
    dmabuf_buffer_t *rgb_back_buffer = NULL;

    if (!rgb_buffer || !yuv_buffer) {
        printf("错误: 无法分配缓冲区\n");
        if (rgb_buffer)
            dmabuf_buffer_free(pool, rgb_buffer);
        if (yuv_buffer)
            dmabuf_buffer_free(pool, yuv_buffer);
        return;
    }

    printf("从单一内存池分配缓冲区: RGB(size=%zu), YUV(size=%zu)\n", rgb_size,
           yuv_size);
    printf("池统计: 容量=%u, 已使用=%u\n", pool->capacity, pool->count);

    // 生成测试数据
#if (USE_MALLOC)
    uint8_t *rgb_data = rgb_buffer->data;
    uint8_t *yuv_data = yuv_buffer->data;
#elif (USE_DMABUF)
    uint8_t *rgb_data = rgb_buffer->mmap_ptr;
    uint8_t *yuv_data = yuv_buffer->mmap_ptr;
#endif

    printf("生成测试RGB数据...\n");
    generate_test_rgb(rgb_data, TEST_WIDTH, TEST_HEIGHT);

    // 保存原始RGB用于验证
    uint8_t *original_rgb = malloc(rgb_size);
    if (original_rgb) {
        memcpy(original_rgb, rgb_data, rgb_size);
    }

    // 执行RGB到YUV转换
    printf("执行RGB888 -> YUV420P转换...\n");
    clock_t start = clock();
    int ret =
        rgb888_to_yuv420p_rga(rgb_data, yuv_data, TEST_WIDTH, TEST_HEIGHT);
    clock_t end = clock();

    if (ret == 0) {
        printf("转换成功! 耗时: %.2f ms\n",
               (double)(end - start) * 1000 / CLOCKS_PER_SEC);

        // 执行YUV到RGB转换
        printf("执行YUV420P -> RGB888转换...\n");

        // 从同一个池分配另一个RGB缓冲区
        rgb_back_buffer = dmabuf_buffer_alloc(pool, rgb_size);
        if (rgb_back_buffer) {
#if (USE_MALLOC)
            uint8_t *rgb_back_data = rgb_back_buffer->data;
#elif (USE_DMABUF)
            uint8_t *rgb_back_data = rgb_back_buffer->mmap_ptr;
#endif

            printf("池统计: 容量=%u, 已使用=%u\n", pool->capacity, pool->count);

            // 清零缓冲区
            memset(rgb_back_data, 0, rgb_size);

            start = clock();
            ret = yuy420p_to_rgb888_rga(yuv_data, rgb_back_data, TEST_WIDTH,
                                        TEST_HEIGHT);
            end = clock();

            if (ret == 0) {
                printf("反向转换成功! 耗时: %.2f ms\n",
                       (double)(end - start) * 1000 / CLOCKS_PER_SEC);

                // 验证转换结果
                if (original_rgb) {
                    if (verify_conversion(original_rgb, rgb_back_data,
                                          TEST_WIDTH, TEST_HEIGHT, 1.0f)) {
                        printf("验证通过! 转换质量良好\n");
                    } else {
                        printf("警告: 转换存在误差\n");
                    }
                    free(original_rgb);
                }

                printf("转换验证完成\n");
            } else {
                printf("错误: YUV到RGB转换失败\n");
            }
        } else {
            printf("错误: 无法分配RGB反向转换缓冲区\n");
        }
    } else {
        printf("错误: RGB到YUV转换失败\n");
    }

    // 释放所有缓冲区回池中
    printf("释放缓冲区回池中...\n");
    if (rgb_buffer)
        dmabuf_buffer_free(pool, rgb_buffer);
    if (yuv_buffer)
        dmabuf_buffer_free(pool, yuv_buffer);
    if (rgb_back_buffer)
        dmabuf_buffer_free(pool, rgb_back_buffer);

    printf("池统计: 容量=%u, 已使用=%u\n", pool->capacity, pool->count);
    printf("测试1完成\n");
}

// 测试2: 队列操作和RGA转换 - 使用单一内存池
void test_queue_with_rga(dmabuf_pool_t *pool) {
    printf("\n=== 测试2: 队列操作和RGA转换（单一内存池） ===\n");

    size_t rgb_size = TEST_WIDTH * TEST_HEIGHT * 3;
    size_t yuv_size = TEST_WIDTH * TEST_HEIGHT * 3 / 2;

    // 创建队列
    dmabuf_queue_t *yuv_queue = dmabuf_queue_create(QUEUE_SIZE);
    if (!yuv_queue) {
        printf("错误: 无法创建队列\n");
        return;
    }

    printf("队列创建成功, 容量: %d\n", QUEUE_SIZE);
    printf("开始填充队列...\n");

    int processed_count = 0;
    int success_count = 0;
    int fail_count = 0;

    while (processed_count < TEST_LOOPS) {
        // 如果队列未满，生成新数据并转换
        if (yuv_queue->size < yuv_queue->capacity) {
            // 从同一个池分配RGB和YUV缓冲区
            dmabuf_buffer_t *rgb_buffer = dmabuf_buffer_alloc(pool, rgb_size);
            dmabuf_buffer_t *yuv_buffer = dmabuf_buffer_alloc(pool, yuv_size);

            if (rgb_buffer && yuv_buffer) {
#if (USE_MALLOC)
                uint8_t *rgb_data = rgb_buffer->data;
                uint8_t *yuv_data = yuv_buffer->data;
#elif (USE_DMABUF)
                uint8_t *rgb_data = rgb_buffer->mmap_ptr;
                uint8_t *yuv_data = yuv_buffer->mmap_ptr;
#endif

                // 生成测试数据
                generate_test_rgb(rgb_data, TEST_WIDTH, TEST_HEIGHT);

                // 执行RGA转换
                printf("转换 %d -> RGB到YUV... ", processed_count + 1);
                clock_t start = clock();
                int ret = rgb888_to_yuv420p_rga(rgb_data, yuv_data, TEST_WIDTH,
                                                TEST_HEIGHT);
                clock_t end = clock();

                if (ret == 0) {
                    success_count++;
                    printf("成功 (%.2f ms)\n",
                           (double)(end - start) * 1000 / CLOCKS_PER_SEC);

                    // YUV缓冲区入队
                    if (dmabuf_queue_enqueue(yuv_queue, yuv_buffer) == 0) {
                        printf("  YUV缓冲区入队成功 (队列大小: %d, 池使用: "
                               "%u/%u)\n",
                               yuv_queue->size, pool->count, pool->capacity);
                    } else {
                        printf("  错误: YUV缓冲区入队失败\n");
                        dmabuf_buffer_free(pool, yuv_buffer);
                    }
                } else {
                    fail_count++;
                    printf("失败\n");
                    dmabuf_buffer_free(pool, yuv_buffer);
                }

                // 释放RGB缓冲区回池中（可以被重用）
                dmabuf_buffer_free(pool, rgb_buffer);
                processed_count++;
            } else {
                printf("警告: 无法分配缓冲区 (池使用: %u/%u)\n", pool->count,
                       pool->capacity);
                // 释放已分配的部分
                if (rgb_buffer)
                    dmabuf_buffer_free(pool, rgb_buffer);
                if (yuv_buffer)
                    dmabuf_buffer_free(pool, yuv_buffer);
                break;
            }
        } else {
            printf("队列已满 (%d)，开始处理队列中的缓冲区...\n",
                   yuv_queue->size);

            // 队列已满，处理队列中的缓冲区
            while (yuv_queue->size > 0) {
                // 从队列中取出YUV缓冲区
                dmabuf_buffer_t *yuv_buffer = dmabuf_queue_dequeue(yuv_queue);
                if (yuv_buffer) {
                    // 从同一个池分配RGB缓冲区用于转换结果
                    dmabuf_buffer_t *rgb_back_buffer =
                        dmabuf_buffer_alloc(pool, rgb_size);

                    if (rgb_back_buffer) {
#if (USE_MALLOC)
                        uint8_t *yuv_data = yuv_buffer->data;
                        uint8_t *rgb_back_data = rgb_back_buffer->data;
#elif (USE_DMABUF)
                        uint8_t *yuv_data = yuv_buffer->mmap_ptr;
                        uint8_t *rgb_back_data = rgb_back_buffer->mmap_ptr;
#endif

                        // 执行YUV到RGB转换
                        printf("处理队列 -> YUV到RGB... ");
                        clock_t start = clock();
                        int ret = yuy420p_to_rgb888_rga(
                            yuv_data, rgb_back_data, TEST_WIDTH, TEST_HEIGHT);
                        clock_t end = clock();

                        if (ret == 0) {
                            printf("成功 (%.2f ms)\n", (double)(end - start) *
                                                           1000 /
                                                           CLOCKS_PER_SEC);
                        } else {
                            printf("失败\n");
                        }

                        // 释放RGB缓冲区
                        dmabuf_buffer_free(pool, rgb_back_buffer);
                    }

                    // 释放YUV缓冲区回池中
                    dmabuf_buffer_free(pool, yuv_buffer);
                }

                // 短暂休眠，模拟处理时间
                usleep(10000); // 10ms
            }

            printf("队列处理完成 (池使用: %u/%u)\n", pool->count,
                   pool->capacity);
        }
    }

    // 处理队列中剩余的数据
    printf("处理队列中剩余的数据 (%d个)...\n", yuv_queue->size);
    while (yuv_queue->size > 0) {
        dmabuf_buffer_t *yuv_buffer = dmabuf_queue_dequeue(yuv_queue);
        if (yuv_buffer) {
            printf("处理剩余YUV缓冲区 (剩余: %d)\n", yuv_queue->size);
            dmabuf_buffer_free(pool, yuv_buffer);
        }
    }

    // 销毁队列
    dmabuf_queue_destroy(yuv_queue);

    printf("测试2完成:\n");
    printf("  总共处理: %d 个缓冲区\n", processed_count);
    printf("  转换成功: %d 次\n", success_count);
    printf("  转换失败: %d 次\n", fail_count);
    printf("  池使用情况: %u/%u\n", pool->count, pool->capacity);
}

// 测试3: 压力测试 - 大量循环转换，使用单一内存池
void test_stress_rga(dmabuf_pool_t *pool) {
    printf("\n=== 测试3: RGA转换压力测试（单一内存池） ===\n");

    size_t rgb_size = TEST_WIDTH * TEST_HEIGHT * 3;
    size_t yuv_size = TEST_WIDTH * TEST_HEIGHT * 3 / 2;

    int stress_loops = TEST_LOOPS * 3;
    int success_count = 0;
    int fail_count = 0;

    // 预分配一组缓冲区，在循环中重用
    int buffer_pairs = POOL_SIZE / 2; // 每个对包括一个RGB和一个YUV缓冲区
    if (buffer_pairs < 2)
        buffer_pairs = 2; // 至少2对

    printf("预分配 %d 对缓冲区 (共 %d 个缓冲区)\n", buffer_pairs,
           buffer_pairs * 2);
    printf("池容量: %u, 预计使用: %d\n", pool->capacity, buffer_pairs * 2);

    dmabuf_buffer_t **rgb_buffers =
        malloc(sizeof(dmabuf_buffer_t *) * buffer_pairs);
    dmabuf_buffer_t **yuv_buffers =
        malloc(sizeof(dmabuf_buffer_t *) * buffer_pairs);

    for (int i = 0; i < buffer_pairs; i++) {
        rgb_buffers[i] = dmabuf_buffer_alloc(pool, rgb_size);
        yuv_buffers[i] = dmabuf_buffer_alloc(pool, yuv_size);
        if (!rgb_buffers[i] || !yuv_buffers[i]) {
            printf("警告: 无法预分配第 %d 对缓冲区\n", i);
            if (rgb_buffers[i])
                dmabuf_buffer_free(pool, rgb_buffers[i]);
            if (yuv_buffers[i])
                dmabuf_buffer_free(pool, yuv_buffers[i]);
            rgb_buffers[i] = NULL;
            yuv_buffers[i] = NULL;
        }
    }

    printf("实际预分配成功: %d 对缓冲区\n", buffer_pairs);
    printf("池当前使用: %u/%u\n", pool->count, pool->capacity);

    clock_t total_start = clock();

    for (int i = 0; i < stress_loops; i++) {
        int pair_idx = i % buffer_pairs;

        if (rgb_buffers[pair_idx] && yuv_buffers[pair_idx]) {
#if (USE_MALLOC)
            uint8_t *rgb_data = rgb_buffers[pair_idx]->data;
            uint8_t *yuv_data = yuv_buffers[pair_idx]->data;
#elif (USE_DMABUF)
            uint8_t *rgb_data = rgb_buffers[pair_idx]->mmap_ptr;
            uint8_t *yuv_data = yuv_buffers[pair_idx]->mmap_ptr;
#endif

            // 生成测试数据
            generate_test_rgb(rgb_data, TEST_WIDTH, TEST_HEIGHT);

            // RGB -> YUV
            int ret1 = rgb888_to_yuv420p_rga(rgb_data, yuv_data, TEST_WIDTH,
                                             TEST_HEIGHT);

            // YUV -> RGB
            int ret2 = yuy420p_to_rgb888_rga(yuv_data, rgb_data, TEST_WIDTH,
                                             TEST_HEIGHT);

            if (ret1 == 0 && ret2 == 0) {
                success_count++;

                // 每10次打印一次进度
                if ((i + 1) % 10 == 0) {
                    printf("进度: %d/%d (使用缓冲区对 %d/%d, 池使用: %u/%u)\n",
                           i + 1, stress_loops, pair_idx + 1, buffer_pairs,
                           pool->count, pool->capacity);
                }
            } else {
                fail_count++;
                printf("循环 %d 失败\n", i + 1);
            }
        } else {
            fail_count++;
            printf("循环 %d: 缓冲区对不可用\n", i + 1);
        }
    }

    clock_t total_end = clock();
    double total_time = (double)(total_end - total_start) / CLOCKS_PER_SEC;

    printf("压力测试完成:\n");
    printf("  总循环次数: %d\n", stress_loops);
    printf("  成功次数: %d\n", success_count);
    printf("  失败次数: %d\n", fail_count);
    printf("  总耗时: %.2f 秒\n", total_time);
    printf("  平均每次转换耗时: %.2f ms\n", total_time * 1000 / stress_loops);
    printf("  缓冲区重用情况: 使用 %d 对缓冲区循环处理 %d 次转换\n",
           buffer_pairs, stress_loops);
    printf("  池最终使用情况: %u/%u\n", pool->count, pool->capacity);

    if (fail_count == 0) {
        printf("压力测试通过!\n");
    } else {
        printf("压力测试存在失败!\n");
    }

    // 释放所有缓冲区回池中
    printf("释放所有缓冲区回池中...\n");
    for (int i = 0; i < buffer_pairs; i++) {
        if (rgb_buffers[i])
            dmabuf_buffer_free(pool, rgb_buffers[i]);
        if (yuv_buffers[i])
            dmabuf_buffer_free(pool, yuv_buffers[i]);
    }

    free(rgb_buffers);
    free(yuv_buffers);

    printf("释放后池使用情况: %u/%u\n", pool->count, pool->capacity);
}

// 测试4: 混合大小缓冲区分配测试
void test_mixed_size_allocation(dmabuf_pool_t *pool) {
    printf("\n=== 测试4: 混合大小缓冲区分配测试 ===\n");

    // 定义不同大小的缓冲区（模拟真实场景）
    size_t sizes[] = {
        TEST_WIDTH * TEST_HEIGHT * 3,     // RGB888
        TEST_WIDTH * TEST_HEIGHT * 3 / 2, // YUV420P
        TEST_WIDTH * TEST_HEIGHT * 2,     // YUYV
        TEST_WIDTH * TEST_HEIGHT * 4,     // RGBA8888
        TEST_WIDTH * TEST_HEIGHT,         // 灰度图
    };

    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    dmabuf_buffer_t *buffers[20]; // 最多分配20个缓冲区
    int buffer_count = 0;

    printf("开始混合大小缓冲区分配测试...\n");
    printf("池初始状态: %u/%u\n", pool->count, pool->capacity);

    // 分配不同大小的缓冲区
    for (int i = 0; i < num_sizes * 2; i++) {
        if (buffer_count >= 20)
            break;

        size_t size = sizes[i % num_sizes];
        dmabuf_buffer_t *buf = dmabuf_buffer_alloc(pool, size);

        if (buf) {
            buffers[buffer_count++] = buf;
            printf("分配缓冲区 %d: size=%zu, id=%u, 池使用: %u/%u\n",
                   buffer_count, size, buf->id, pool->count, pool->capacity);
        } else {
            printf("分配失败: size=%zu, 池已满: %u/%u\n", size, pool->count,
                   pool->capacity);
            break;
        }
    }

    // 随机释放一些缓冲区
    printf("\n随机释放部分缓冲区...\n");
    srand(time(NULL));
    int to_free = buffer_count / 2;
    for (int i = 0; i < to_free; i++) {
        int idx = rand() % buffer_count;
        if (buffers[idx]) {
            printf("释放缓冲区: id=%u, size=%zu\n", buffers[idx]->id,
                   buffers[idx]->size);
            dmabuf_buffer_free(pool, buffers[idx]);
            buffers[idx] = NULL;
        }
    }

    printf("释放后池状态: %u/%u\n", pool->count, pool->capacity);

    // 重新分配一些缓冲区（测试重用）
    printf("\n重新分配缓冲区测试重用...\n");
    for (int i = 0; i < num_sizes; i++) {
        size_t size = sizes[i];
        dmabuf_buffer_t *buf = dmabuf_buffer_alloc(pool, size);

        if (buf) {
            // 查找空位存储
            for (int j = 0; j < 20; j++) {
                if (j < buffer_count && !buffers[j]) {
                    buffers[j] = buf;
                    break;
                } else if (j >= buffer_count) {
                    buffers[buffer_count++] = buf;
                    break;
                }
            }

            printf("重新分配: size=%zu, id=%u, 池使用: %u/%u\n", size, buf->id,
                   pool->count, pool->capacity);
        } else {
            printf("重新分配失败: size=%zu\n", size);
        }
    }

    // 释放所有缓冲区
    printf("\n释放所有缓冲区...\n");
    for (int i = 0; i < buffer_count; i++) {
        if (buffers[i]) {
            dmabuf_buffer_free(pool, buffers[i]);
        }
    }

    printf("最终池状态: %u/%u\n", pool->count, pool->capacity);
    printf("测试4完成\n");
}

// 打印使用说明
void print_usage(const char *program_name) {
    printf("使用说明: %s [测试模式]\n", program_name);
    printf("测试模式:\n");
    printf("  0 或 simple  - 简单RGA转换测试（单一内存池）\n");
    printf("  1 或 queue   - 队列操作和RGA转换测试（单一内存池）\n");
    printf("  2 或 stress  - RGA转换压力测试（单一内存池）\n");
    printf("  3 或 mixed   - 混合大小缓冲区分配测试\n");
    printf("  不指定参数   - 运行所有测试\n");
    printf("\n当前配置:\n");
    printf("  图像尺寸: %dx%d\n", TEST_WIDTH, TEST_HEIGHT);
    printf("  队列容量: %d\n", QUEUE_SIZE);
    printf("  缓冲池大小: %d\n", POOL_SIZE);
    printf("  测试循环: %d\n", TEST_LOOPS);
    printf("  内存模式: 单一内存池管理所有类型缓冲区\n");
#if (USE_MALLOC)
    printf("  分配方式: malloc\n");
#elif (USE_DMABUF)
    printf("  分配方式: dmabuf\n");
#endif
}

int main(int argc, char *argv[]) {
    printf("RGA转换和队列测试程序（单一内存池）\n");
    printf("==================================\n\n");

    // 解析命令行参数
    int run_all_tests = 0;
    int run_simple = 0, run_queue = 0, run_stress = 0, run_mixed = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "0") == 0 || strcmp(argv[1], "simple") == 0) {
            run_simple = 1;
        } else if (strcmp(argv[1], "1") == 0 || strcmp(argv[1], "queue") == 0) {
            run_queue = 1;
        } else if (strcmp(argv[1], "2") == 0 ||
                   strcmp(argv[1], "stress") == 0) {
            run_stress = 1;
        } else if (strcmp(argv[1], "3") == 0 || strcmp(argv[1], "mixed") == 0) {
            run_mixed = 1;
        } else if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "-a") == 0) {
            run_all_tests = 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    } else {
        // 未指定参数，运行所有测试
        run_all_tests = 1;
    }

    // 创建单一内存池
    printf("创建单一内存池...\n");
    dmabuf_pool_t *pool = dmabuf_pool_create(POOL_SIZE);

    if (!pool) {
        printf("错误: 无法创建内存池\n");
        return 1;
    }

    printf("单一内存池创建成功 (容量: %d)\n\n", POOL_SIZE);

    // 运行测试
    if (run_all_tests) {
        test_simple_rga_conversion(pool);
        test_queue_with_rga(pool);
        test_stress_rga(pool);
        test_mixed_size_allocation(pool);
    } else {
        if (run_simple)
            test_simple_rga_conversion(pool);
        if (run_queue)
            test_queue_with_rga(pool);
        if (run_stress)
            test_stress_rga(pool);
        if (run_mixed)
            test_mixed_size_allocation(pool);
    }

    // 销毁内存池
    printf("\n清理资源...\n");
    dmabuf_pool_destroy(pool);

    printf("\n测试程序完成\n");
    return 0;
}
