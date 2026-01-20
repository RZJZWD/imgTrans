#include "display_rgb.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// LVGL头文件
#include "lvgl/lvgl.h"

// 全局变量
static lv_display_t *disp = NULL;
static lv_obj_t *img_obj = NULL;
static lv_obj_t *info_label = NULL;
static lv_obj_t *exit_label = NULL;
static lv_image_dsc_t img_dsc;
static uint8_t *img_data = NULL;
static int running = 1;
static DisplayLabelConfig current_config;

// 默认配置
static const DisplayLabelConfig default_config = {.show_info_label = 1,
                                                  .show_exit_label = 0,
                                                  .info_label_x = 10,
                                                  .info_label_y = 10,
                                                  .info_text = NULL};

// 内部函数声明
static int load_rgb_file_internal(const char *filename, int *width, int *height,
                                  uint8_t **data);
static void create_test_image_internal(int width, int height, uint8_t **data);
static void sig_handler(int sig);
static void create_labels(lv_obj_t *scr, int width, int height);

// 加载 RGB 文件（内部函数）
static int load_rgb_file_internal(const char *filename, int *width, int *height,
                                  uint8_t **data) {
    FILE *fp = fopen(filename, "rb");
    if (!fp)
        return -1;

    fread(width, sizeof(int), 1, fp);
    fread(height, sizeof(int), 1, fp);

    int size = (*width) * (*height) * 3;
    *data = (uint8_t *)malloc(size);
    if (*data == NULL) {
        fclose(fp);
        return -1;
    }

    fread(*data, 1, size, fp);
    fclose(fp);

    return 0;
}

// 创建测试图像（内部函数）
static void create_test_image_internal(int width, int height, uint8_t **data) {
    int size = width * height * 3;
    *data = (uint8_t *)malloc(size);

    // 创建渐变图案
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            (*data)[idx] = (x * 255) / width;      // R
            (*data)[idx + 1] = (y * 255) / height; // G
            (*data)[idx + 2] = 128;                // B
        }
    }
}

// 创建标签
static void create_labels(lv_obj_t *scr, int width, int height) {
    // 清理旧的标签
    if (info_label != NULL) {
        lv_obj_del(info_label);
        info_label = NULL;
    }
    if (exit_label != NULL) {
        lv_obj_del(exit_label);
        exit_label = NULL;
    }

    // 创建信息标签
    if (current_config.show_info_label) {
        info_label = lv_label_create(scr);
        const char *text = current_config.info_text;
        if (text == NULL) {
            text = "UVC Camera Display\n%d x %d";
            lv_label_set_text_fmt(info_label, text, width, height);
        } else {
            lv_label_set_text(info_label, text);
        }
        lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(info_label, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(info_label, lv_color_black(), 0);
        lv_obj_set_style_pad_all(info_label, 10, 0);
        lv_obj_align(info_label, LV_ALIGN_TOP_LEFT, current_config.info_label_x,
                     current_config.info_label_y);
    }

    // 创建退出提示标签
    if (current_config.show_exit_label) {
        exit_label = lv_label_create(scr);
        lv_label_set_text(exit_label, "Press ESC to exit");
        lv_obj_set_style_text_color(exit_label, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(exit_label, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(exit_label, lv_color_black(), 0);
        lv_obj_set_style_pad_all(exit_label, 10, 0);
        lv_obj_align(exit_label, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    }
}

// 初始化RGB显示系统
int display_rgb_init(DisplayLabelConfig *config) {
    printf("初始化LVGL显示系统\n");

    // 设置配置
    if (config != NULL) {
        memcpy(&current_config, config, sizeof(DisplayLabelConfig));
    } else {
        memcpy(&current_config, &default_config, sizeof(DisplayLabelConfig));
    }

    // 1. 初始化LVGL
    lv_init();

    // 2. 创建Linux DRM显示设备
    disp = lv_linux_drm_create();
    if (!disp) {
        printf("创建DRM显示失败\n");
        return -1;
    }

    // 3. 设置DRM设备文件
    const char *drm_devices[] = {"/dev/dri/card0", "/dev/dri/card1",
                                 "/dev/dri/card2", NULL};

    int drm_found = 0;
    for (int i = 0; drm_devices[i]; i++) {
        if (access(drm_devices[i], F_OK) == 0) {
            lv_linux_drm_set_file(disp, drm_devices[i], -1);
            printf("使用DRM设备: %s\n", drm_devices[i]);
            drm_found = 1;
            break;
        }
    }

    if (!drm_found) {
        printf("未找到可用的DRM设备\n");
        lv_display_delete(disp);
        disp = NULL;
        return -1;
    }

    return 0;
}

// 内部辅助函数：设置图像数据
static int set_image_data(uint8_t *data, int width, int height) {
    // 清理旧的图像数据
    if (img_data != NULL) {
        free(img_data);
        img_data = NULL;
    }

    // 设置新的图像数据
    img_data = data;

    // 更新图像描述符
    memset(&img_dsc, 0, sizeof(img_dsc));
    img_dsc.header.w = width;
    img_dsc.header.h = height;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    img_dsc.data_size = width * height * 3;
    img_dsc.data = img_data;

    // 获取屏幕对象
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // 创建或更新图像对象
    if (img_obj == NULL) {
        // 创建新的图像对象
        img_obj = lv_image_create(scr);
        lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
    }

    // 设置图像源
    lv_image_set_src(img_obj, &img_dsc);

    // 创建或更新标签
    create_labels(scr, width, height);

    // printf("显示图像尺寸: %dx%d\n", width, height);
    return 0;
}

// 从文件加载并显示RGB图像
int display_rgb_from_file(const char *filename) {
    if (disp == NULL) {
        printf("显示系统未初始化\n");
        return -1;
    }

    int width, height;
    uint8_t *data = NULL;

    if (load_rgb_file_internal(filename, &width, &height, &data) < 0) {
        printf("无法加载图像文件: %s\n", filename);
        return -1;
    }

    return set_image_data(data, width, height);
}

// 从内存缓冲区显示RGB图像
int display_rgb_from_buffer(uint8_t *data, int width, int height) {
    if (disp == NULL) {
        printf("显示系统未初始化\n");
        return -1;
    }

    if (data == NULL || width <= 0 || height <= 0) {
        printf("无效的图像参数\n");
        return -1;
    }

    // 复制图像数据
    int size = width * height * 3;
    uint8_t *copy_data = (uint8_t *)malloc(size);
    if (copy_data == NULL) {
        printf("内存分配失败\n");
        return -1;
    }

    memcpy(copy_data, data, size);
    return set_image_data(copy_data, width, height);
}

// 创建并显示测试图像
int display_rgb_test_image(int width, int height) {
    if (disp == NULL) {
        printf("显示系统未初始化\n");
        return -1;
    }

    if (width <= 0 || height <= 0) {
        width = 640;
        height = 480;
    }

    uint8_t *data = NULL;
    create_test_image_internal(width, height, &data);

    return set_image_data(data, width, height);
}

// 更新信息标签文本
int display_rgb_update_label(const char *text, ...) {
    if (info_label == NULL) {
        return -1;
    }

    if (text != NULL) {
        va_list args;
        char buffer[256];

        va_start(args, text);
        vsnprintf(buffer, sizeof(buffer), text, args);
        va_end(args);

        lv_label_set_text(info_label, buffer);
    }

    return 0;
}

// 运行显示主循环
void display_rgb_run(void) {
    if (disp == NULL) {
        printf("显示系统未初始化\n");
        return;
    }

    if (img_obj == NULL) {
        printf("没有图像显示\n");
        return;
    }

    lv_timer_handler();
    usleep(5000); // 5ms
}

// 停止显示循环
void display_rgb_stop(void) { running = 0; }

// 获取当前显示状态
int display_rgb_is_running(void) { return running; }

// 清理显示资源
void display_rgb_cleanup(void) {
    printf("清理显示资源\n");

    // 停止运行
    running = 0;

    // 释放图像数据
    if (img_data != NULL) {
        free(img_data);
        img_data = NULL;
    }

    // 清理LVGL资源
    if (disp != NULL) {
        lv_deinit();
        disp = NULL;
        img_obj = NULL;
        info_label = NULL;
        exit_label = NULL;
    }
}