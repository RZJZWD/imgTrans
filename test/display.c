#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// LVGL 头文件
#include <lvgl/lvgl.h>

// JPEG 解码库
#include <jpeglib.h>
#include <setjmp.h>

// 全局变量
static lv_display_t *disp = NULL;
static lv_obj_t *img_obj = NULL;
static lv_image_dsc_t img_dsc;
static uint8_t *img_data = NULL;
static int running = 1;

// 信号处理
static void sig_handler(int sig) {
    running = 0;
    printf("\n接收到信号 %d，正在退出...\n", sig);
}

// JPEG 错误处理结构体
struct my_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

typedef struct my_error_mgr *my_error_ptr;

// JPEG 错误处理回调
static void my_error_exit(j_common_ptr cinfo) {
    my_error_ptr myerr = (my_error_ptr)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(myerr->setjmp_buffer, 1);
}

// 加载 JPEG 文件并解码为 RGB888
static int load_jpeg_file(const char *filename, int *width, int *height,
                          uint8_t **data) {
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;
    FILE *infile = NULL;
    JSAMPARRAY buffer = NULL;
    int row_stride;
    uint8_t *rgb_data = NULL;
    int success = 0;

    // 打开文件
    if (!(infile = fopen(filename, "rb"))) {
        fprintf(stderr, "无法打开 JPEG 文件: %s\n", filename);
        return -1;
    }

    // 初始化 JPEG 解压缩对象
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    // 设置错误处理跳转点
    if (setjmp(jerr.setjmp_buffer)) {
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);

    // 读取 JPEG 头信息
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        fprintf(stderr, "无效的 JPEG 文件头: %s\n", filename);
        goto cleanup;
    }

    // 配置解压缩参数
    cinfo.out_color_space = JCS_RGB;
    cinfo.output_components = 3;

    // 开始解压缩
    jpeg_start_decompress(&cinfo);

    // 获取图像尺寸
    *width = cinfo.output_width;
    *height = cinfo.output_height;

    // 验证尺寸有效性
    if (*width <= 0 || *height <= 0 || *width > 16384 || *height > 16384) {
        fprintf(stderr, "无效的 JPEG 尺寸: %dx%d\n", *width, *height);
        goto cleanup;
    }

    // 分配内存
    row_stride = cinfo.output_width * cinfo.output_components;
    size_t data_size = (size_t)*width * *height * 3;

    if (!(rgb_data = malloc(data_size))) {
        fprintf(stderr, "内存分配失败 (%zu 字节)\n", data_size);
        goto cleanup;
    }

    // 分配行缓冲区
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                        row_stride, 1);
    if (!buffer) {
        fprintf(stderr, "行缓冲区分配失败\n");
        goto cleanup;
    }

    // 逐行读取图像数据
    uint8_t *dst = rgb_data;
    while (cinfo.output_scanline < cinfo.output_height) {
        JDIMENSION num_scanlines = jpeg_read_scanlines(&cinfo, buffer, 1);
        if (num_scanlines != 1) {
            fprintf(stderr, "扫描线读取错误\n");
            goto cleanup;
        }
        memcpy(dst, buffer[0], row_stride);
        dst += row_stride;
    }

    // 完成解压缩
    if (!jpeg_finish_decompress(&cinfo)) {
        fprintf(stderr, "解压缩未完成\n");
        goto cleanup;
    }

    *data = rgb_data;
    success = 1;

cleanup:
    // 清理资源
    if (cinfo.err) {
        jpeg_destroy_decompress(&cinfo);
    }
    if (infile) {
        fclose(infile);
    }

    if (!success && rgb_data) {
        free(rgb_data);
        rgb_data = NULL;
    }

    return success ? 0 : -1;
}

// 加载 RGB 文件
static int load_rgb_file(const char *filename, int *width, int *height,
                         uint8_t **data) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "无法打开 RGB 文件: %s\n", filename);
        return -1;
    }

    // 读取宽度和高度
    if (fread(width, sizeof(int), 1, file) != 1 ||
        fread(height, sizeof(int), 1, file) != 1) {
        fprintf(stderr, "读取 RGB 文件头失败: %s\n", filename);
        fclose(file);
        return -1;
    }

    // 验证尺寸有效性
    if (*width <= 0 || *height <= 0 || *width > 16384 || *height > 16384) {
        fprintf(stderr, "无效的 RGB 尺寸: %dx%d\n", *width, *height);
        fclose(file);
        return -1;
    }

    // 分配内存并读取数据
    size_t data_size = (size_t)(*width) * (*height) * 3;
    uint8_t *rgb_data = malloc(data_size);
    if (!rgb_data) {
        fprintf(stderr, "内存分配失败 (%zu 字节)\n", data_size);
        fclose(file);
        return -1;
    }

    if (fread(rgb_data, 1, data_size, file) != data_size) {
        fprintf(stderr, "读取 RGB 数据失败: %s\n", filename);
        free(rgb_data);
        fclose(file);
        return -1;
    }

    fclose(file);
    *data = rgb_data;
    return 0;
}

// 创建测试图像
static void create_test_image(int width, int height, uint8_t **data) {
    size_t size = (size_t)width * height * 3;
    *data = malloc(size);
    if (!*data) {
        fprintf(stderr, "测试图像内存分配失败\n");
        return;
    }

    // 创建彩色渐变图案
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            (*data)[idx] = (uint8_t)((x * 255) / width);                    // R
            (*data)[idx + 1] = (uint8_t)((y * 255) / height);               // G
            (*data)[idx + 2] = (uint8_t)((x + y) * 128 / (width + height)); // B
        }
    }
}

int main(int argc, char *argv[]) {
    const char *filename = argc > 1 ? argv[1] : "test.jpg";
    int width = 800, height = 600;
    int is_rgb = 0;

    // 检查文件扩展名是否为.rgb
    const char *ext = strrchr(filename, '.');
    if (ext && (strcmp(ext, ".rgb") == 0 || strcmp(ext, ".RGB") == 0)) {
        is_rgb = 1;
    }

    printf("图像查看器\n");
    printf("用法: %s [image.jpg|image.rgb]\n", argv[0]);

    // 1. 加载图像文件
    if (is_rgb) {
        if (load_rgb_file(filename, &width, &height, &img_data) != 0) {
            printf("无法加载RGB文件 '%s'，创建测试图像\n", filename);
            create_test_image(width, height, &img_data);
        } else {
            printf("成功加载RGB: %s (%dx%d)\n", filename, width, height);
        }
    } else {
        if (load_jpeg_file(filename, &width, &height, &img_data) != 0) {
            printf("无法加载图像 '%s'，创建测试图像\n", filename);
            create_test_image(width, height, &img_data);
        } else {
            printf("成功加载图像: %s (%dx%d)\n", filename, width, height);
        }
    }

    // 2. 初始化 LVGL
    lv_init();

    // 3. 创建显示设备
    disp = lv_linux_drm_create();
    if (!disp) {
        fprintf(stderr, "无法创建 DRM 显示\n");
        free(img_data);
        return EXIT_FAILURE;
    }

    // 4. 设置 DRM 设备
    const char *drm_path = "/dev/dri/card0";
    if (access(drm_path, F_OK) != 0) {
        drm_path = "/dev/dri/renderD128";
    }
    lv_linux_drm_set_file(disp, drm_path, -1);
    printf("使用 DRM 设备: %s\n", drm_path);

    // 5. 设置图像描述符
    memset(&img_dsc, 0, sizeof(img_dsc));
    img_dsc.header.w = (uint32_t)width;
    img_dsc.header.h = (uint32_t)height;
    img_dsc.data_size = (uint32_t)(width * height * 3);
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    img_dsc.data = img_data;

    // 6. 创建 UI
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x303030), 0);

    // 创建图像对象
    img_obj = lv_image_create(scr);
    lv_image_set_src(img_obj, &img_dsc);
    lv_obj_center(img_obj);

    // 添加标题
    lv_obj_t *title = lv_label_create(scr);
    if (is_rgb) {
        lv_label_set_text_fmt(title, "RGB 图像查看器 - %dx%d", width, height);
    } else {
        lv_label_set_text_fmt(title, "JPEG 图像查看器 - %dx%d", width, height);
    }
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 添加状态栏
    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "按 ESC 退出 | LVGL 图像查看器");
    lv_obj_set_style_text_color(status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 7. 设置信号处理
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 8. 主循环
    printf("显示已启动，按 ESC 或 Ctrl+C 退出\n");
    while (running) {
        lv_timer_handler();
        usleep(5000); // 5ms

        // 检查退出键
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        if (key == LV_KEY_ESC) {
            running = 0;
        }
    }

    // 9. 清理资源
    printf("正在清理资源...\n");
    free(img_data);
    lv_deinit();

    printf("程序已退出\n");
    return EXIT_SUCCESS;
}