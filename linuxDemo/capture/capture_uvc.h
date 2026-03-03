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
/**
 * @brief 初始化UVC摄像头
 * @param width 捕获宽
 * @param height 捕获高
 * @param color 捕获颜色格式
 * @param alloc_buf_from_pool 从这个池申请缓冲区，池由外部创建
 * @param frames 缓冲区个数,也就是内部帧队列数量
 * @param framerate 帧率
 * @return 成功返回0 失败返回-1
 */
int capture_uvc_init(uint32_t width, uint32_t height, enum capture_color color,
                     dmabuf_pool_t *alloc_buf_from_pool, int frames,
                     int framerate);
/**
 * @brief
 * 捕获一帧，成功时管理返回的缓冲区引用计数，对调用者默认加一次引用，失败管理传入的缓冲区引用计数
 * @param next_buffer 下一个入队的缓冲区
 * @return dmabuf_buffer_t* 本次捕获获取的数据缓冲区，失败NULL
 */
dmabuf_buffer_t *capture_uvc_captureImg(dmabuf_buffer_t *next_buffer);
/**
 * @brief 清理v4l2资源
 * @param alloc_buf_from_pool 初始化时使用的缓冲池，内部将缓冲区释放回缓冲池
 */
void capture_uvc_clean(dmabuf_pool_t *alloc_buf_from_pool);
/**
 * @brief 设置摄像头参数
 * @param enable_auto_exposure 使能自动曝光，true:忽略固定曝光时间
 * @param fixed_exposure_time 固定曝光时间，100 微秒单位，其中值 1 代表 1/10000
 * 秒，100代表10ms
 * @param enable_dynamic_framerate 使能动态帧率，true:忽略固定帧率
 */
void capture_uvc_set_camera(bool enable_auto_exposure, int fixed_exposure_time,
                            bool enable_dynamic_framerate);

size_t capture_uvc_get_v4l2buf_size(void);

#ifdef __cplusplus
}
#endif

#endif //_CAPTURE_UVC_H