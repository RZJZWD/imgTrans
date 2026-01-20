#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// LVGL 头文件
#include <lvgl/lvgl.h>
// #include "lvgl/drivers/linux/lv_linux_drm.h"

// 全局变量
static lv_display_t *disp = NULL;
static lv_obj_t *img_obj = NULL;
static lv_image_dsc_t img_dsc;
static uint8_t *img_data = NULL;
static int running = 1;

// 信号处理
static void sig_handler(int sig) { running = 0; }

// 加载 RGB 文件
int load_rgb_file(const char *filename, int *width, int *height,
                  uint8_t **data) {
    FILE *fp = fopen(filename, "rb");
    if (!fp)
        return -1;

    fread(width, sizeof(int), 1, fp);
    fread(height, sizeof(int), 1, fp);

    int size = (*width) * (*height) * 3;
    *data = malloc(size);
    fread(*data, 1, size, fp);
    fclose(fp);

    return 0;
}

// 创建测试图像（如果文件加载失败）
void create_test_image(int width, int height, uint8_t **data) {
    int size = width * height * 3;
    *data = malloc(size);

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

// 更新图像数据（可以用于动态更新）
static void update_image(lv_timer_t *timer) {
    static int counter = 0;
    counter++;

    // 每100次更新一次显示
    if (counter % 100 == 0) {
        lv_obj_invalidate(img_obj);
    }
}

// 键盘事件回调
static void keyboard_event_cb(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        running = 0;
    }
}

int main(int argc, char *argv[]) {
    const char *filename = argc > 1 ? argv[1] : "capture.rgb";

    printf("LVGL DRM 显示程序\n");

    // 1. 加载图像
    int width, height;

    if (load_rgb_file(filename, &width, &height, &img_data) < 0) {
        printf("无法加载图像文件，创建测试图像\n");
        width = 640;
        height = 480;
        create_test_image(width, height, &img_data);
    }

    printf("图像尺寸: %dx%d\n", width, height);

    // 2. 初始化 LVGL
    lv_init();

    // 3. 创建 Linux DRM 显示设备
    // 注意：这里使用 LVGL 9.x 的 Linux DRM 驱动 API
    disp = lv_linux_drm_create();
    if (!disp) {
        printf("创建 DRM 显示失败\n");
        free(img_data);
        return -1;
    }

    // 4. 设置 DRM 设备文件
    // 尝试常见的 DRM 设备
    const char *drm_devices[] = {"/dev/dri/card0", "/dev/dri/card1",
                                 "/dev/dri/card2", NULL};

    int drm_found = 0;
    for (int i = 0; drm_devices[i]; i++) {
        if (access(drm_devices[i], F_OK) == 0) {
            lv_linux_drm_set_file(disp, drm_devices[i], -1);
            printf("使用 DRM 设备: %s\n", drm_devices[i]);
            drm_found = 1;
            break;
        }
    }

    if (!drm_found) {
        printf("未找到可用的 DRM 设备\n");
        lv_display_delete(disp);
        free(img_data);
        return -1;
    }

    // 5. 设置图像描述符
    memset(&img_dsc, 0, sizeof(img_dsc));
    img_dsc.header.w = width;
    img_dsc.header.h = height;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    img_dsc.data_size = width * height * 3;
    img_dsc.data = img_data;

    // 6. 创建 UI
    lv_obj_t *scr = lv_screen_active();

    // 设置黑色背景
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // 创建图像对象
    img_obj = lv_image_create(scr);
    lv_image_set_src(img_obj, &img_dsc);
    lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);

    // 创建信息标签
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text_fmt(label, "UVC Camera Display\n%d x %d", width, height);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_pad_all(label, 10, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);

    // 创建退出提示
    lv_obj_t *exit_label = lv_label_create(scr);
    lv_label_set_text(exit_label, "Press ESC to exit");
    lv_obj_set_style_text_color(exit_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(exit_label, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(exit_label, lv_color_black(), 0);
    lv_obj_set_style_pad_all(exit_label, 10, 0);
    lv_obj_align(exit_label, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    // 注册键盘事件（如果使用输入设备）
    lv_obj_add_event_cb(scr, keyboard_event_cb, LV_EVENT_KEY, NULL);

    // 7. 信号处理
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 8. 创建更新定时器
    lv_timer_create(update_image, 16, NULL); // ~60fps

    printf("显示已启动，按 ESC 或 Ctrl+C 退出\n");

    // 9. 主循环
    unsigned long frame_count = 0;
    clock_t start_time = clock();

    while (running) {
        lv_timer_handler();
        usleep(5000); // 5ms

        frame_count++;

        // 显示帧率信息
        if (frame_count % 200 == 0) {
            clock_t current_time = clock();
            double elapsed =
                (double)(current_time - start_time) / CLOCKS_PER_SEC;
            double fps = 200.0 / elapsed;
            printf("帧率: %.1f fps\n", fps);
            start_time = current_time;
        }
    }

    // 10. 清理资源
    printf("正在退出...\n");

    free(img_data);
    lv_deinit();

    printf("程序结束\n");
    return 0;
}