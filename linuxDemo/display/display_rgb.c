#include "display_rgb.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// LVGL头文件
#include "lvgl/lvgl.h"

// ======================== 全局变量 ========================
static lv_display_t *disp = NULL;
static lv_obj_t *img_obj = NULL;
static uint8_t *img_data = NULL;
static int running = 1;

// 新的UI控件变量
static lv_obj_t *sw = NULL;             // 开关控件
static lv_obj_t *btn_photo = NULL;      // 拍照按钮
static lv_obj_t *btn_record = NULL;     // 录制按钮
static lv_obj_t *label_filename = NULL; // 文件名标签
static lv_obj_t *label_info = NULL;     // 信息标签
static lv_obj_t *ui_container = NULL;   // UI容器

// 状态变量
static VideoState video_state = {0};
static PhotoCallback photo_callback = NULL;
static RecordStartCallback record_start_callback = NULL;
static RecordStopCallback record_stop_callback = NULL;

// FPS计算相关
static struct timespec fps_last_time = {0};
static int fps_frame_count = 0;

// 内部函数声明
// 从文件加载RGB
static int load_rgb_file_internal(const char *filename, int *width, int *height,
                                  uint8_t **data);
// 创建测试图片
static void create_test_image_internal(int width, int height, uint8_t **data);
// 绘制UI
static void create_ui(lv_obj_t *src);
// 控制回调
static void sw_event_handler(lv_event_t *e);
static void btn_photo_event_handler(lv_event_t *e);
static void btn_record_event_handler(lv_event_t *e);
// 辅助函数：设置图像数据
static int set_image_data(uint8_t *data, int width, int height);
// fps计算
static void update_fps_counter(void);
static void reset_fps_counter(void);
// 统一更新UI信息
static void update_ui_info(void);
// 初始化RGB显示系统
int display_rgb_init() {
    printf("初始化LVGL显示系统\n");

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
    // 初始化视频状态
    video_state.file_counter = 1;
    video_state.is_recording = 0;
    video_state.width = 1920;
    video_state.height = 1080;
    video_state.fps = 0.0;
    snprintf(video_state.current_filename, sizeof(video_state.current_filename),
             "video_001.mp4");

    // 重置FPS计数器
    reset_fps_counter();
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

// ======================== 设置回调函数 ========================
void display_rgb_set_callbacks(PhotoCallback photo_cb,
                               RecordStartCallback record_start_cb,
                               RecordStopCallback record_stop_cb) {
    photo_callback = photo_cb;
    record_start_callback = record_start_cb;
    record_stop_callback = record_stop_cb;
}
// ======================== 更新视频信息 ========================
void display_rgb_update_video_info(int width, int height, float fps) {
    video_state.width = width;
    video_state.height = height;
    video_state.fps = fps;

    if (label_info) {
        char info_text[100];
        snprintf(info_text, sizeof(info_text),
                 "Res: %dx%d | Frame Rate: %.1f FPS", width, height, fps);
        lv_label_set_text(label_info, info_text);
    }
}

// ======================== 更新信息标签文本 ========================
// 此函数现在更新UI中的信息标签
int display_rgb_update_label(const char *text, ...) {
    if (label_info == NULL) {
        return -1;
    }

    if (text != NULL) {
        va_list args;
        char buffer[256];

        va_start(args, text);
        vsnprintf(buffer, sizeof(buffer), text, args);
        va_end(args);

        lv_label_set_text(label_info, buffer);
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
    // 更新FPS计数器
    update_fps_counter();
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
        ui_container = NULL;
        sw = NULL;
        btn_photo = NULL;
        btn_record = NULL;
        label_filename = NULL;
        label_info = NULL;
    }
}

// ======================== 内部函数 ========================
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

static void create_ui(lv_obj_t *src) {
    // 如果UI存在，清理UI
    if (ui_container) {
        lv_obj_del(ui_container);
        ui_container = NULL;
    }

    // 创建半透明背景的容器
    ui_container = lv_obj_create(src);
    lv_obj_set_size(ui_container, 300, 300);
    lv_obj_align(ui_container, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_set_style_bg_color(ui_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ui_container, LV_OPA_50, 0);
    lv_obj_set_style_border_width(ui_container, 0, 0);
    lv_obj_set_style_pad_all(ui_container, 10, 0);
    lv_obj_set_flex_flow(ui_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 1.创建开关
    sw = lv_switch_create(ui_container);
    lv_obj_set_size(sw, 60, 30);
    lv_obj_add_event_cb(sw, sw_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_state(sw, LV_STATE_CHECKED); // 默认开启
    lv_obj_t *sw_label = lv_label_create(ui_container);
    lv_label_set_text(sw_label, "UI Control:");
    lv_obj_set_style_text_color(sw_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(sw_label, &lv_font_montserrat_20, 0);

    // 2. 创建拍照按钮
    btn_photo = lv_btn_create(ui_container);
    lv_obj_set_size(btn_photo, 200, 40);
    lv_obj_add_event_cb(btn_photo, btn_photo_event_handler, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *btn_photo_label = lv_label_create(btn_photo);
    lv_label_set_text(btn_photo_label, "Take Photo");
    lv_obj_set_style_text_color(btn_photo_label, lv_color_white(), 0);
    lv_obj_center(btn_photo_label);

    // 3. 创建录制按钮
    btn_record = lv_btn_create(ui_container);
    lv_obj_set_size(btn_record, 200, 40);
    lv_obj_add_event_cb(btn_record, btn_record_event_handler, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *btn_record_label = lv_label_create(btn_record);
    lv_label_set_text(btn_record_label, "Start Recording");
    lv_obj_set_style_text_color(btn_record_label, lv_color_white(), 0);
    lv_obj_center(btn_record_label);

    // 4. 创建文件名标签
    label_filename = lv_label_create(ui_container);
    lv_label_set_text(label_filename, "Video File: video_001.mp4");
    lv_obj_set_style_text_color(label_filename, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label_filename, &lv_font_montserrat_16, 0);

    // 5. 创建信息标签
    label_info = lv_label_create(ui_container);
    lv_label_set_text(label_info, "Res: 1920x1080 | Frame Rate: 30 FPS");
    lv_obj_set_style_text_color(label_info, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(label_info, &lv_font_montserrat_16, 0);

    // 默认显示UI
    lv_obj_clear_flag(ui_container, LV_OBJ_FLAG_HIDDEN);
}
// ======================== UI回调函数 ========================
static void sw_event_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);

        if (state && ui_container) {
            // 开关开启：显示UI容器
            lv_obj_clear_flag(ui_container, LV_OBJ_FLAG_HIDDEN);
        } else if (ui_container) {
            // 开关关闭：隐藏UI容器
            lv_obj_add_flag(ui_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void btn_photo_event_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        printf("拍照按钮被点击\n");

        // 触发拍照回调
        if (photo_callback) {
            photo_callback();
        }
    }
}

static void btn_record_event_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        if (video_state.is_recording == 0) {
            // 开始录制
            video_state.is_recording = 1;

            // 更新按钮文本和颜色
            lv_obj_t *label = lv_obj_get_child(btn_record, 0);
            lv_label_set_text(label, "Stop Recording");
            lv_obj_set_style_bg_color(btn_record, lv_color_hex(0xFF3333), 0);

            // 生成新文件名
            snprintf(video_state.current_filename,
                     sizeof(video_state.current_filename), "video_%03d.mp4",
                     video_state.file_counter);

            // 更新文件名标签
            char display_text[100];
            snprintf(display_text, sizeof(display_text), "Video File: %s",
                     video_state.current_filename);
            lv_label_set_text(label_filename, display_text);

            printf("开始录制视频: %s\n", video_state.current_filename);

            // 触发开始录制回调
            if (record_start_callback) {
                record_start_callback(video_state.current_filename);
            }

        } else {
            // 停止录制
            video_state.is_recording = 0;

            // 更新按钮文本和颜色
            lv_obj_t *label = lv_obj_get_child(btn_record, 0);
            lv_label_set_text(label, "Start Recording");
            lv_obj_set_style_bg_color(btn_record,
                                      lv_palette_main(LV_PALETTE_BLUE), 0);

            printf("停止录制\n");
            video_state.file_counter++;

            // 触发停止录制回调
            if (record_stop_callback) {
                record_stop_callback();
            }
        }
    }
}
// 内部辅助函数：设置图像数据
static int set_image_data(uint8_t *data, int width, int height) {
    // 获取屏幕对象
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    // 创建或更新图像对象
    if (img_obj == NULL) {
        // 创建新的图像对象
        img_obj = lv_image_create(scr);
        lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
    }
    // 清理旧的图像数据
    if (img_data != NULL) {
        free(img_data);
        img_data = NULL;
    }

    // 设置新的图像数据
    img_data = data;

    // 更新图像描述符
    static lv_image_dsc_t img_dsc;
    memset(&img_dsc, 0, sizeof(lv_image_dsc_t)); // 确保清零
    img_dsc.header.w = width;
    img_dsc.header.h = height;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    img_dsc.data_size = width * height * 3;
    img_dsc.data = img_data;

    // 设置图像源
    lv_image_set_src(img_obj, &img_dsc);

    // 创建UI（只在第一次时创建）
    if (ui_container == NULL) {
        create_ui(scr);
    }

    // 保存分辨率到状态表
    video_state.width = width;
    video_state.height = height;
    // 更新FPS计数器（每次设置图像时调用）
    update_fps_counter();

    // 更新UI信息
    update_ui_info();

    // lv_refr_now(NULL);
    return 0;
}
// 统一更新UI信息
static void update_ui_info(void) {
    if (label_info) {
        char info_text[100];
        snprintf(info_text, sizeof(info_text), "Res: %dx%d | Display FPS: %.1f",
                 video_state.width, video_state.height, video_state.fps);
        lv_label_set_text(label_info, info_text);
    }
}
// FPS计算函数
static void update_fps_counter(void) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    if (fps_last_time.tv_sec == 0) {
        // 第一次调用，初始化
        fps_last_time = current_time;
        fps_frame_count = 0;
        return;
    }

    fps_frame_count++;

    // 计算时间差（秒）
    double elapsed = (current_time.tv_sec - fps_last_time.tv_sec) +
                     (current_time.tv_nsec - fps_last_time.tv_nsec) / 1e9;

    // 每秒更新一次FPS显示
    if (elapsed >= 1.0) {
        video_state.fps = fps_frame_count / elapsed;
        // 重置计数
        fps_frame_count = 0;
        fps_last_time = current_time;
    }
}

static void reset_fps_counter(void) {
    fps_last_time.tv_sec = 0;
    fps_last_time.tv_nsec = 0;
    fps_frame_count = 0;
}