#include "launcher_ui.h"
#include "lvgl_init.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    const char *img_path = "./img_trans"; // 默认路径
    const char *location = "未知位置";

    if (argc >= 2)
        img_path = argv[1];
    if (argc >= 3)
        location = argv[2];

    lv_display_t *disp = lvgl_drm_init();
    if (!disp) {
        fprintf(stderr, "LVGL 初始化失败\n");
        return 1;
    }

    launcher_ui_create(img_path, location);

    while (1) {
        lv_timer_handler();
        usleep(5000);
        lv_tick_inc(5);
    }

    lvgl_deinit();
    return 0;
}