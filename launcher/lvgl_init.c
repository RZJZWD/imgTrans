#include "lvgl_init.h"
#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// LVGL 驱动头文件（需根据实际路径调整）
// #include "lv_drivers/display/drm.h"
// #include "lv_drivers/indev/evdev.h"

/* ---------- 内部辅助 ---------- */
static int is_touch_device(const char *device_path) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0)
        return 0;

    char name[256] = {0};
    int is_touch = 0;
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
        char lower[256];
        strncpy(lower, name, sizeof(lower));
        for (int i = 0; lower[i]; i++)
            lower[i] = tolower(lower[i]);

        const char *keywords[] = {"ft5x06", "ft5", "ft6", // FT 系列
                                  "touch",  "ts",         // 通用触摸屏
                                  "goodix", "gt9",        // 汇顶
                                  "ilitek", "ili",        // 奕力
                                  NULL};
        for (int i = 0; keywords[i]; i++) {
            if (strstr(lower, keywords[i])) {
                is_touch = 1;
                break;
            }
        }
    }
    close(fd);
    return is_touch;
}

static void init_touch_input(lv_display_t *disp) {
    const char *touch_devs[] = {"/dev/input/event0", "/dev/input/event1",
                                "/dev/input/event2", NULL};

    for (int i = 0; touch_devs[i]; i++) {
        if (access(touch_devs[i], F_OK) != 0)
            continue;
        if (!is_touch_device(touch_devs[i]))
            continue;

        lv_indev_t *indev =
            lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_devs[i]);
        if (indev) {
            lv_indev_set_display(indev, disp);
            return; // 绑定第一个可用触摸设备
        }
    }
}

/* ---------- 公开接口 ---------- */
lv_display_t *lvgl_drm_init(void) {
    lv_init();

    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        fprintf(stderr, "Failed to create DRM display\n");
        return NULL;
    }

    // 尝试多个 DRM 设备
    const char *drm_devs[] = {"/dev/dri/card0", "/dev/dri/card1",
                              "/dev/dri/card2", NULL};
    int found = 0;
    for (int i = 0; drm_devs[i]; i++) {
        if (access(drm_devs[i], F_OK) == 0) {
            lv_linux_drm_set_file(disp, drm_devs[i], -1);
            found = 1;
            break;
        }
    }
    if (!found) {
        lv_display_delete(disp);
        return NULL;
    }

    init_touch_input(disp);
    return disp;
}

void lvgl_deinit(void) { lv_deinit(); }