#ifndef _CAPTURE_UVC_H
#define _CAPTURE_UVC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 颜色格式
enum capture_color {
    CAP_NONE,
    CAP_YUYV,
    CAP_JPEG,
    CAP_NUMS,
};

/**
 * @brief 初始化UVC摄像头，默认捕获YUYV格式，方便后面转为jpeg
 * @param width 图像宽
 * @param height 图像高
 * @return int 成功返回0 失败返回1
 */
int capture_uvc_init(int width, int height, enum capture_color color);

int capture_uvc_captureImg(void);

void capture_uvc_clean(void);

uint8_t *capture_uvc_getRGBbuffer(void);
#ifdef __cplusplus
}
#endif

#endif //_CAPTURE_UVC_H