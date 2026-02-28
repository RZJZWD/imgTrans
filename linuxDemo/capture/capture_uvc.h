#ifndef _CAPTURE_UVC_H
#define _CAPTURE_UVC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dmabuf_manager.h"
#include <stdint.h>

// 颜色格式
enum capture_color {
    CAP_NONE,
    CAP_YUYV,
    CAP_JPEG,
    CAP_NUMS,
};
#if (USE_MALLOC)
/**
 * @brief 初始化UVC摄像头，默认捕获YUYV格式，方便后面转为jpeg
 * @param width 图像宽
 * @param height 图像高
 * @return int 成功返回0 失败返回-1
 */
int capture_uvc_init(int width, int height, enum capture_color color);

int capture_uvc_captureImg(void);

void capture_uvc_clean(void);

uint8_t *capture_uvc_getRGBbuffer(void);

// uvc摄像头原始数据，在捕获下一帧前一直在
uint8_t *capture_getRawbuffer(uint32_t *raw_buf_size);
#elif (USE_DMABUF)
// dmabuf优化专用
/**
 * @brief 初始化UVC摄像头
 * @param width 捕获宽
 * @param height 捕获高
 * @param color 捕获颜色格式
 * @param alloc_buf_from_pool 从这个池申请缓冲区，池由外部创建
 * @param frames 缓冲区个数,也就是内部帧队列数量
 * @return 成功返回0 失败返回-1
 */
int capture_uvc_init_dmabuf(uint32_t width, uint32_t height,
                            enum capture_color color,
                            dmabuf_pool_t *alloc_buf_from_pool, int frames);
/**
 * @brief 捕获一帧，成功时管理返回的缓冲区引用计数，失败管理传入的缓冲区引用计数
 * @param next_buffer 下一个入队的缓冲区
 * @return dmabuf_buffer_t* 本次捕获获取的数据缓冲区，失败NULL
 */
dmabuf_buffer_t *capture_uvc_captureImg_dmabuf(dmabuf_buffer_t *next_buffer);
/**
 * @brief 清理v4l2资源
 * @param alloc_buf_from_pool 初始化时使用的缓冲池，内部将缓冲区释放回缓冲池
 */
void capture_uvc_clean_dmabuf(dmabuf_pool_t *alloc_buf_from_pool);

size_t capture_uvc_get_v4l2buf_size(void);
#endif

#ifdef __cplusplus
}
#endif

#endif //_CAPTURE_UVC_H