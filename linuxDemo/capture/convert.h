#ifndef _CONVERT_H
#define _CONVERT_H

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

void save_ppm(const char *filename, uint8_t *rgb, int width, int height);
void save_rgb(const char *filename, uint8_t *rgb, int width, int height);
void yuyv_to_rgb(uint8_t *yuyv, uint8_t *rgb, int width, int height);

/*
 * JPEG解码函数：将内存中的JPEG数据解码为RGB格式
 *
 * 参数:
 *   jpeg_data   - 指向JPEG压缩数据的指针
 *   jpeg_size   - JPEG数据的大小（字节）
 *   rgb_buffer  - 输出缓冲区，存放解码后的RGB数据
 *   width       - 期望的图像宽度（像素）
 *   height      - 期望的图像高度（像素）
 *
 * 返回值:
 *   0   - 成功
 *   -1  - 失败
 */
int jpeg_get_version();
int jpeg_to_yuv420p_turbo(uint8_t *jpeg_data, unsigned long jpeg_size,
                          uint8_t *yuv420_data, int width, int height);
int jpeg_to_rgb888_turbo(uint8_t *jpeg_data, unsigned long jpeg_size,
                         uint8_t *bgr_data, int width, int height);
int rgb888_to_yuv420p_rga(void *rgb_data, void *yuv_data, int width,
                          int height);
int yuv420p_to_rgb888_rga(void *yuv_data, void *rgb_data, int width,
                          int height);
int yuyv422_to_yuv420p_sw(const uint8_t *yuyv, uint8_t *yuv420p, int width,
                          int height);
int rgb888_to_yuv420p_sw(void *rgb_data, void *yuv_data, int width, int height);
int yuv420p_to_rgb888_sw(void *yuv_data, void *rgb_data, int width, int height);
int yuv420p_to_bgr888_sw(void *yuv_data, void *rgb_data, int width, int height);
int yuyv422_to_yuv420p_neno(uint8_t *yuyv, uint8_t *yuv420p, int width,
                            int height);
// int rgb888_to_yuv420p_neno(void *rgb_data, void *yuv_data, int width,
//                            int height);
int yuv420p_to_rgb888_neno(void *yuv_data, void *rgb_data, int width,
                           int height);
int yuv420p_to_bgr888_neno(void *yuv_data, void *rgb_data, int width,
                           int height);
#ifdef __cplusplus
}
#endif

#endif