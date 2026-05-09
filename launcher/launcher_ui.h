#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

void launcher_ui_create(const char *img_trans_path, const char *location);

#ifdef __cplusplus
}
#endif
#endif