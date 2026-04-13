#ifndef _CONVERT_H
#define _CONVERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dmabuf_manager.h"
#include <stdint.h>

/* ==================== 转换模式枚举 ==================== */
/* 根据内存分配模式自动选择默认转换模式：
 * DMABUF_ALLOC_MODE == 0 → malloc 模式 → 默认用 SIMD 软件加速
 * DMABUF_ALLOC_MODE == 1 → dmabuf 模式 → 默认用 RGA 硬件加速
 * convert_mode_t convert_mode = (DMABUF_ALLOC_MODE == 0) ?
 * CONVERT_MODE_CPU_SIMD : CONVERT_MODE_RGA;
 */

typedef enum {
    CONVERT_MODE_CPU_SCALAR, /* 纯 C 标量实现（参考基准） */
    CONVERT_MODE_CPU_SIMD,   /* NEON SIMD 加速实现 */
    CONVERT_MODE_RGA         /* RGA 硬件加速（使用fd导入） */
} convert_mode_t;

/* ==================== JPEG 解码相关函数 ==================== */
int jpeg_get_version(void);
int jpeg_to_yuv420p_turbo(uint8_t *jpeg_data, unsigned long jpeg_size,
                          uint8_t *yuv420_data, int width, int height);
int jpeg_to_rgb888_turbo(uint8_t *jpeg_data, unsigned long jpeg_size,
                         uint8_t *bgr_data, int width, int height);

/* ==================== 统一转换入口 ==================== */

/**
 * @brief YUYV422 转换为 YUV420P
 * @param src_buf  源缓冲区（dmabuf_buffer_t 指针）
 * @param dst_buf  目标缓冲区（dmabuf_buffer_t 指针）
 * @param width    图像宽度
 * @param height   图像高度
 * @param mode     转换模式
 * @return 成功返回 0，失败返回 -1
 */
int yuyv422_to_yuv420p(dmabuf_buffer_t *src_buf, dmabuf_buffer_t *dst_buf,
                       int width, int height, convert_mode_t mode);

/**
 * @brief YUV420P 转换为 RGB888
 * @param src_buf  源缓冲区（dmabuf_buffer_t 指针）
 * @param dst_buf  目标缓冲区（dmabuf_buffer_t 指针）
 * @param width    图像宽度
 * @param height   图像高度
 * @param mode     转换模式
 * @return 成功返回 0，失败返回 -1
 */
int yuv420p_to_rgb888(dmabuf_buffer_t *src_buf, dmabuf_buffer_t *dst_buf,
                      int width, int height, convert_mode_t mode);

/**
 * @brief YUV420P 转换为 BGR888
 * @param src_buf  源缓冲区（dmabuf_buffer_t 指针）
 * @param dst_buf  目标缓冲区（dmabuf_buffer_t 指针）
 * @param width    图像宽度
 * @param height   图像高度
 * @param mode     转换模式
 * @return 成功返回 0，失败返回 -1
 */
int yuv420p_to_bgr888(dmabuf_buffer_t *src_buf, dmabuf_buffer_t *dst_buf,
                      int width, int height, convert_mode_t mode);

/* ==================== 原独立底层实现，保证兼容性 ==================== */

/* 纯软件标量版本 */

int yuyv422_to_yuv420p_sw(const uint8_t *yuyv, uint8_t *yuv420p, int width,
                          int height);
int yuv420p_to_rgb888_sw(void *yuv_data, void *rgb_data, int width, int height);
int yuv420p_to_bgr888_sw(void *yuv_data, void *bgr_data, int width, int height);

/* SIMD 加速版本 */

int yuyv422_to_yuv420p_neno(uint8_t *yuyv, uint8_t *yuv420p, int width,
                            int height);
int yuv420p_to_rgb888_neno(void *yuv_data, void *rgb_data, int width,
                           int height);
int yuv420p_to_bgr888_neno(void *yuv_data, void *bgr_data, int width,
                           int height);

/* RGA 底层转换函数：只接收 fd 和图像参数 */

int yuyv422_to_yuv420p_rga(int src_fd, int dst_fd, int width, int height);
int yuv420p_to_rgb888_rga(int src_fd, int dst_fd, int width, int height);
int yuv420p_to_bgr888_rga(int src_fd, int dst_fd, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* _CONVERT_H */