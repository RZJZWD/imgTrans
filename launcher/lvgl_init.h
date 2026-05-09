#ifndef LVGL_INIT_H
#define LVGL_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @brief 初始化 LVGL + DRM 显示 + 触摸输入
 * @return 成功返回 LVGL 显示对象指针，失败返回 NULL
 */
lv_display_t *lvgl_drm_init(void);

/**
 * @brief 反初始化 LVGL，释放资源
 */
void lvgl_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // LVGL_INIT_H