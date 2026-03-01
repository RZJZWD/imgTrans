#include "display_rgb.h"
#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
// LVGL头文件
#include "lvgl/lvgl.h"

// ======================== 全局变量 ========================
static lv_display_t *disp = NULL;
static lv_obj_t *img_obj = NULL;
static uint8_t *img_data = NULL;

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
static uint32_t fps_last_time = 0;
static uint32_t fps_frame_count = 0;
float fps_frame = 0.0;

// 触摸状态（精简版）
static int touch_enabled = 0; // 触摸是否启用

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
static void touch_event_handler(lv_event_t *e); // 新增触摸事件处理器

// 辅助函数：设置图像数据
static int set_image_data(uint8_t *data, int width, int height);
// fps计算
static void update_fps_counter(void);
static void reset_fps_counter(void);
// 统一更新UI信息
static void update_ui_info(void);
// 初始化触摸输入设备
static void init_touch_input(lv_display_t *display);
static int is_touch_device(const char *device_path);

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
    init_touch_input(disp);

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

    // 内部会根据宏决定是否拷贝，所以统一调用即可
    return set_image_data(data, width, height);
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
    // 更新LVGL内部时钟
    static uint32_t last_tick = 0;
    uint32_t now = lv_tick_get();
    if (now - last_tick > 5) { // 约5ms
        lv_tick_inc(5);
        last_tick = now;
    }
    usleep(1000); // 1ms
}
// ======================== 控制图像显示/隐藏 ========================
int display_rgb_show_image(int show) {
    if (img_obj == NULL) {
        printf("图像对象未创建\n");
        return -1;
    }

    if (show) {
        lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
        printf("图像已显示\n");
    } else {
        lv_obj_add_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
        printf("图像已隐藏\n");
    }

    // 同步开关状态（如果开关存在）
    if (sw) {
        if (show) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
    }

    return 0;
}
// ======================== 获取图像显示状态 ========================
int display_rgb_get_image_state(void) {
    if (img_obj == NULL) {
        return -1; // 图像未创建
    }

    if (lv_obj_has_flag(img_obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0; // 图像隐藏中
    } else {
        return 1; // 图像显示中
    }
}

// ======================== 清除当前显示的图像 ========================
int display_rgb_clear_image(void) {
    if (img_obj == NULL) {
        printf("图像对象未创建\n");
        return -1;
    }

    // 隐藏图像
    lv_obj_add_flag(img_obj, LV_OBJ_FLAG_HIDDEN);

    // 清除图像源
    lv_image_set_src(img_obj, NULL);

    // 释放图像数据
    if (img_data != NULL) {
        free(img_data);
        img_data = NULL;
    }

    printf("图像已清除\n");
    return 0;
}

// 清理显示资源
void display_rgb_cleanup(void) {
    printf("清理显示资源\n");

#if DISPLAY_SHOW_ZERO_COPY
    // 零拷贝由外部释放
#else
    // 拷贝模式：已在 set_image_data中处理
    // 释放图像数据
    if (img_data != NULL) {
        free(img_data);
        img_data = NULL;
    }
#endif
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
    // // 如果UI存在，清理UI
    // if (ui_container) {
    //     lv_obj_del(ui_container);
    //     ui_container = NULL;
    // }

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
    lv_label_set_text(sw_label, "Image Show:");
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
    lv_obj_remove_flag(ui_container, LV_OBJ_FLAG_HIDDEN);
}
// ======================== UI回调函数 ========================
static void sw_event_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (state) {
        // 开关开启：显示图像
        if (img_obj) {
            lv_obj_remove_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
            printf("图像显示已开启\n");
        }
    } else {
        // 开关关闭：隐藏图像
        if (img_obj) {
            lv_obj_add_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
            printf("图像显示已关闭\n");
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
// ======================== 触摸事件处理器 ========================
static void touch_event_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // 获取触摸坐标
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    // 只打印点击和按下事件，避免过多输出
    switch (code) {
    case LV_EVENT_PRESSED:
        printf("触摸按下: 坐标(%d, %d)\n", point.x, point.y);
        break;

    case LV_EVENT_CLICKED:
        printf("触摸点击: 坐标(%d, %d)\n", point.x, point.y);
        break;

    case LV_EVENT_RELEASED:
        printf("触摸释放: 坐标(%d, %d)\n", point.x, point.y);
        break;

    default:
        // 其他事件不打印
        break;
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

        // 默认显示图像，通过开关关闭显示
        lv_obj_remove_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    }
#if DISPLAY_SHOW_ZERO_COPY
    // 零拷贝模式：直接使用传入的指针，不拷贝
    // 注意：必须确保上层在显示完成前不释放 data
    img_data = data; // 假设 img_data 是 static 或全局变量，仅保存指针
#else
    // 拷贝模式：先释放旧数据，再拷贝新数据
    if (img_data != NULL) {
        free(img_data);
        img_data = NULL;
    }
    int size = width * height * 3;
    img_data = (uint8_t *)malloc(size);
    if (img_data == NULL) {
        printf("内存分配失败\n");
        return -1;
    }
    memcpy(img_data, data, size);
#endif
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
                 video_state.width, video_state.height, fps_frame);
        lv_label_set_text(label_info, info_text);
    }
}
// FPS计算函数
static void update_fps_counter(void) {
    fps_frame_count++;
    uint32_t now = lv_tick_get();

    // 计算时间差（秒）
    double elapsed = now - fps_last_time;

    // 每秒更新一次FPS显示
    if (elapsed >= 1000) {
        fps_frame = (fps_frame_count * 1000) / elapsed;
        // 重置计数
        fps_frame_count = 0;
        fps_last_time = now;
    }
}

static void reset_fps_counter(void) {
    fps_frame_count = 0;
    fps_last_time = 0;
    fps_frame = 0;
}
// ======================== 初始化触摸输入设备 ========================
static void init_touch_input(lv_display_t *display) {
    printf("初始化触摸输入设备...\n");

    const char *touch_devices[] = {"/dev/input/event0", "/dev/input/event1",
                                   "/dev/input/event2", NULL};

    lv_indev_t *touch_indev = NULL;

    for (int i = 0; touch_devices[i]; i++) {
        if (access(touch_devices[i], F_OK) == 0) {
            printf("尝试初始化触摸设备: %s\n", touch_devices[i]);

            if (!is_touch_device(touch_devices[i])) {
                printf("不是触摸设备，跳过\n");
                continue;
            }

            touch_indev =
                lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_devices[i]);
            if (touch_indev) {
                lv_indev_set_display(touch_indev, display);
                touch_enabled = 1;
                printf("触摸屏已启用，触摸时会打印坐标\n");
                break;
            } else {
                printf("无法打开触摸设备: %s\n", touch_devices[i]);
                lv_indev_delete(touch_indev);
            }
        }
    }

    // 在init_touch_input函数中，创建设备后添加：
    if (touch_enabled) {
        // 为屏幕添加事件处理器
        lv_obj_t *scr = lv_display_get_screen_active(display);
        lv_obj_add_event_cb(scr, touch_event_handler, LV_EVENT_ALL, NULL);
    } else if (!touch_enabled) {
        printf("未找到触摸屏设备\n");
        // 可以在这里添加鼠标模拟，但为了精简暂时不实现
        printf("触摸功能未启用\n");
    }
}
static int is_touch_device(const char *device_path) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    char name[256] = "unknown";
    int is_touch = 0;

    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
        printf("设备 %s 名称: %s\n", device_path, name);

        // 传小写
        char lower_name[256];
        strncpy(lower_name, name, sizeof(lower_name));
        for (int i = 0; lower_name[i]; i++) {
            lower_name[i] = tolower(lower_name[i]);
        }

        // 检查是否包含触摸屏关键词
        const char *touch_keywords[] = {"ft5x06", "ft5", "ft6", // FT系列
                                        "touch",  "ts",         // 通用触摸屏
                                        "goodix", "gt9",        // 汇顶
                                        "ilitek", "ili",        // 奕力
                                        NULL};

        for (int i = 0; touch_keywords[i]; i++) {
            if (strstr(lower_name, touch_keywords[i])) {
                printf("✓ 匹配触摸屏关键词: %s\n", touch_keywords[i]);
                is_touch = 1;
                break;
            }
        }
    }
    close(fd);
    return is_touch;
}