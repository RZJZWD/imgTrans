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
typedef struct {
    // 曝光控制
    bool enable_auto_exposure;     // 是否自动曝光
    int exposure_time;             // 手动曝光时间（单位依赖驱动）
    bool enable_dynamic_framerate; // 动态帧率（曝光优先级）

    // 白平衡
    bool enable_auto_white_balance; // 自动白平衡
    int white_balance_temperature;  // 手动色温（K）

    // 增益
    bool enable_auto_gain; // 自动增益
    int gain;              // 手动增益值

    // 基本图像控制
    int brightness; // 亮度 (0-255)
    int contrast;   // 对比度 (0-255)
    int saturation; // 饱和度 (0-100)
    int sharpness;  // 锐度 (0-7)
} capture_params_t;
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
 * @param params 参数结构体
 */
int capture_uvc_set_params(capture_params_t *params);

size_t capture_uvc_get_v4l2buf_size(void);
enum capture_color capture_uvc_get_color(void);
#ifdef __cplusplus
}
#endif

#endif //_CAPTURE_UVC_H