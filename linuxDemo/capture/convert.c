#include "convert.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
// jpeg解码
#include <jpeglib.h>
#include <setjmp.h>

// rga
#include <RgaApi.h>
#include <im2d.h>
// jpeg变量和错误处理
struct my_error_mgr {
    // 标准jpeg库错误处理结构
    struct jpeg_error_mgr pub;
    // 非局部跳转的缓冲区
    jmp_buf setjmp_buffer;
};
/* 自定义错误处理函数 - 当JPEG库遇到致命错误时调用 */
void my_error_exit(j_common_ptr cinfo) {
    // 将通用指针转换为我们的错误管理器类型
    struct my_error_mgr *myerr = (struct my_error_mgr *)cinfo->err;

    // 输出标准错误信息（到stderr）
    (*cinfo->err->output_message)(cinfo);

    // 跳转到之前设置的setjmp点，返回1表示错误发生
    // 这会使程序控制流跳回到setjmp调用处
    longjmp(myerr->setjmp_buffer, 1);
}
int jpeg_to_rgb(uint8_t *jpeg_data, unsigned long jpeg_size, uint8_t *rgb,
                int width, int height) {
    // 1. 声明JPEG解压所需的结构体
    struct jpeg_decompress_struct cinfo; // JPEG解压主结构体
    struct my_error_mgr jerr;            // 自定义错误处理结构
    JSAMPROW row_pointer[1];             // 指向一行像素数据的指针
    int row_stride;                      // 每行数据的字节数

    // 错误处理初始化
    cinfo.err = jpeg_std_error(&jerr.pub); // 设置标准错误处理到自定义结构
    jerr.pub.error_exit = my_error_exit;   // 设置错误处理函数为自定义函数

    // 设置错误恢复点
    // setjmp()在这里设置一个恢复点，如果后续发生错误并通过longjmp跳转回来
    // 则返回值为非零（这里是1）；否则返回0表示正常执行路径
    if (setjmp(jerr.setjmp_buffer)) {
        // 当longjmp跳转回这里时，表示发生了错误
        // 清理JPEG解压对象占用的资源
        jpeg_destroy_decompress(&cinfo);
        // 返回错误代码
        return -1;
    }

    // jpeg解压初始化
    // 1.初始化解压对象
    jpeg_create_decompress(&cinfo);
    // 2.设置数据源
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    // 3.查看头
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        // jpeg·头无效
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    // 4.验证尺寸
    if (cinfo.image_width != (JDIMENSION)width ||
        cinfo.image_height != (JDIMENSION)height) {
        // 尺寸不匹配，打印警告信息
        fprintf(stderr, "JPEG尺寸不匹配: 期望 %dx%d, 实际 %dx%d\n", width,
                height, (int)cinfo.image_width, (int)cinfo.image_height);
        // 清理资源并返回错误
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    // 5.设置解压参数
    // cinfo.out_color_space = JCS_RGB;
    // 解决LVGL小端问题
    cinfo.out_color_space = JCS_EXT_BGR;
    // 开始解压
    // 1.初始化解压，准备解压扫描线
    jpeg_start_decompress(&cinfo);
    // 2.计算图像 字节数/行
    // 宽 * 输出颜色组成（灰度为1,rgb为3）
    row_stride = cinfo.output_width * cinfo.out_color_components;
    // 3.逐行解压
    while (cinfo.output_scanline < cinfo.output_height) {
        // 计算当前行在输出缓冲区的位置，设置目标地址
        row_pointer[0] = (JSAMPROW)&rgb[cinfo.output_scanline * row_stride];

        // 读取一行扫描线数据
        if (jpeg_read_scanlines(&cinfo, row_pointer, 1) != 1) {
            // 清理资源并返回错误
            jpeg_destroy_decompress(&cinfo);
            return -1;
        }
    }

    // 解压完成
    jpeg_finish_decompress(&cinfo);

    // 清理资源
    jpeg_destroy_decompress(&cinfo);

    return 0;
}
int jpeg_to_yuv(uint8_t *jpeg_data, unsigned long jpeg_size, uint8_t *yuv,
                int width, int height) {
    // 1. 声明JPEG解压所需的结构体
    struct jpeg_decompress_struct cinfo; // JPEG解压主结构体
    struct my_error_mgr jerr;            // 自定义错误处理结构
    JSAMPROW row_pointer[1];             // 指向一行像素数据的指针
    int row_stride;                      // 每行数据的字节数

    // 错误处理初始化
    cinfo.err = jpeg_std_error(&jerr.pub); // 设置标准错误处理到自定义结构
    jerr.pub.error_exit = my_error_exit;   // 设置错误处理函数为自定义函数

    // 设置错误恢复点
    // setjmp()在这里设置一个恢复点，如果后续发生错误并通过longjmp跳转回来
    // 则返回值为非零（这里是1）；否则返回0表示正常执行路径
    if (setjmp(jerr.setjmp_buffer)) {
        // 当longjmp跳转回这里时，表示发生了错误
        // 清理JPEG解压对象占用的资源
        jpeg_destroy_decompress(&cinfo);
        // 返回错误代码
        return -1;
    }

    // jpeg解压初始化
    // 1.初始化解压对象
    jpeg_create_decompress(&cinfo);
    // 2.设置数据源
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    // 3.查看头
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        // jpeg·头无效
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    // 4.验证尺寸
    if (cinfo.image_width != (JDIMENSION)width ||
        cinfo.image_height != (JDIMENSION)height) {
        // 尺寸不匹配，打印警告信息
        fprintf(stderr, "JPEG尺寸不匹配: 期望 %dx%d, 实际 %dx%d\n", width,
                height, (int)cinfo.image_width, (int)cinfo.image_height);
        // 清理资源并返回错误
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    // 5.设置解压参数 yuv444
    cinfo.out_color_space = JCS_YCbCr;
    // 开始解压
    // 1.初始化解压，准备解压扫描线
    jpeg_start_decompress(&cinfo);
    // 2.计算图像 字节数/行
    // 宽 * 输出颜色组成（灰度为1,rgb为3）
    row_stride = cinfo.output_width * cinfo.output_components;
    // 3.逐行解压
    while (cinfo.output_scanline < cinfo.output_height) {
        // 计算当前行在输出缓冲区的位置，设置目标地址
        row_pointer[0] = (JSAMPROW)(yuv + cinfo.output_scanline * row_stride);

        // 读取一行扫描线数据
        if (jpeg_read_scanlines(&cinfo, row_pointer, 1) != 1) {
            // 清理资源并返回错误
            jpeg_destroy_decompress(&cinfo);
            return -1;
        }
    }

    // 解压完成
    jpeg_finish_decompress(&cinfo);

    // 清理资源
    jpeg_destroy_decompress(&cinfo);

    return 0;
}
// 简单YUYV转RGB转换
void yuyv_to_rgb(uint8_t *yuyv, uint8_t *rgb, int width, int height) {
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j += 2) {
            int y0 = yuyv[i * width * 2 + j * 2];
            int u = yuyv[i * width * 2 + j * 2 + 1];
            int y1 = yuyv[i * width * 2 + j * 2 + 2];
            int v = yuyv[i * width * 2 + j * 2 + 3];

            // // 简化的转换公式
            // int r0 = y0 + 1.402 * (v - 128);
            // int g0 = y0 - 0.344 * (u - 128) - 0.714 * (v - 128);
            // int b0 = y0 + 1.772 * (u - 128);

            // int r1 = y1 + 1.402 * (v - 128);
            // int g1 = y1 - 0.344 * (u - 128) - 0.714 * (v - 128);
            // int b1 = y1 + 1.772 * (u - 128);

            // 简化的转换公式,交换RGB->BGR，解决lvgl显示问题
            int b0 = y0 + 1.402 * (v - 128);
            int g0 = y0 - 0.344 * (u - 128) - 0.714 * (v - 128);
            int r0 = y0 + 1.772 * (u - 128);

            int b1 = y1 + 1.402 * (v - 128);
            int g1 = y1 - 0.344 * (u - 128) - 0.714 * (v - 128);
            int r1 = y1 + 1.772 * (u - 128);

// 限制范围
#define CLAMP(x) (x < 0 ? 0 : (x > 255 ? 255 : x))
            r0 = CLAMP(r0);
            g0 = CLAMP(g0);
            b0 = CLAMP(b0);
            r1 = CLAMP(r1);
            g1 = CLAMP(g1);
            b1 = CLAMP(b1);

            // 存储RGB
            int idx0 = (i * width + j) * 3;
            rgb[idx0] = r0;
            rgb[idx0 + 1] = g0;
            rgb[idx0 + 2] = b0;

            int idx1 = (i * width + j + 1) * 3;
            rgb[idx1] = r1;
            rgb[idx1 + 1] = g1;
            rgb[idx1 + 2] = b1;
        }
    }
}
// YUYV转YUV420P函数
void yuyv_to_yuv420p(const uint8_t *yuyv, uint8_t *yuv420p, int width,
                     int height) {
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);
    uint8_t *y_plane = yuv420p;
    uint8_t *u_plane = yuv420p + y_size;
    uint8_t *v_plane = yuv420p + y_size + uv_size;

    for (int i = 0; i < height; i += 2) {
        for (int j = 0; j < width; j += 2) {
            int idx = i * width * 2 + j * 2;

            // Y分量
            y_plane[i * width + j] = yuyv[idx];
            y_plane[i * width + j + 1] = yuyv[idx + 2];
            y_plane[(i + 1) * width + j] = yuyv[idx + width * 2];
            y_plane[(i + 1) * width + j + 1] = yuyv[idx + width * 2 + 2];

            // U和V分量（每2x2像素共享）
            u_plane[(i / 2) * (width / 2) + (j / 2)] = yuyv[idx + 1];
            v_plane[(i / 2) * (width / 2) + (j / 2)] = yuyv[idx + 3];
        }
    }
}
// 保存为PPM格式（简单易读）
void save_ppm(const char *filename, uint8_t *rgb, int width, int height) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("打开PPM文件失败");
        return;
    }

    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, width * height * 3, fp);
    fclose(fp);

    printf("已保存: %s (尺寸: %dx%d)\n", filename, width, height);
}

// 保存为原始RGB格式
void save_rgb(const char *filename, uint8_t *rgb, int width, int height) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("打开RGB文件失败");
        return;
    }

    // 写入宽度和高度（用于显示程序读取）
    fwrite(&width, sizeof(int), 1, fp);
    fwrite(&height, sizeof(int), 1, fp);
    fwrite(rgb, 1, width * height * 3, fp);
    fclose(fp);

    printf("已保存: %s (尺寸: %dx%d)\n", filename, width, height);
}

// int rgb888_to_yuv420p(uint8_t *rgb_data, uint8_t *yuv_data, int width,
//                       int height) {
//     int ret = IM_STATUS_SUCCESS;
//     rga_buffer_t src_img, dst_img;
//     rga_buffer_handle_t src_handle, dst_handle;

//     // 验证输入参数
//     if (!rgb_data || !yuv_data || width <= 0 || height <= 0) {
//         return IM_STATUS_INVALID_PARAM;
//     }

//     // 计算缓冲区大小
//     int rgb_buf_size = width * height * 3;     // RGB888: 每个像素3字节
//     int yuv_buf_size = width * height * 3 / 2; // YUV420P: 每个像素1.5字节

//     // 导入源RGB缓冲区为RGA句柄
//     src_handle = importbuffer_virtualaddr(rgb_data, rgb_buf_size);
//     if (src_handle == NULL) {
//         return IM_STATUS_FAILED;
//     }

//     // 导入目标YUV缓冲区为RGA句柄
//     dst_handle = importbuffer_virtualaddr(yuv_data, yuv_buf_size);
//     if (dst_handle == NULL) {
//         releasebuffer_handle(src_handle);
//         return IM_STATUS_FAILED;
//     }

//     // 包装缓冲区为RGA图像结构体
//     src_img = wrapbuffer_handle(src_handle, width, height,
//     RK_FORMAT_BGR_888); dst_img =
//         wrapbuffer_handle(dst_handle, width, height, RK_FORMAT_YCbCr_420_P);

//     // 验证包装结果
//     if (src_img.width == 0 || src_img.height == 0 || dst_img.width == 0 ||
//         dst_img.height == 0) {
//         releasebuffer_handle(src_handle);
//         releasebuffer_handle(dst_handle);
//         return IM_STATUS_INVALID_PARAM;
//     }

//     // 设置转换模式参数
//     im_rect src_rect = {0, 0, width, height};
//     im_rect dst_rect = {0, 0, width, height};

//     // 执行颜色空间转换：RGB888 -> YUV420P
//     // 注意：imcvtcolor需要传入源和目标格式
//     ret = imcvtcolor(src_img, dst_img,
//                      RK_FORMAT_BGR_888,     // 源格式
//                      RK_FORMAT_YCbCr_420_P, // 目标格式
//                      0,                     // 转换模式，0表示标准转换
//                      0,                     // 同步标志
//                      &src_rect,             // 源区域
//                      &dst_rect);            // 目标区域

//     // 释放缓冲区句柄
//     releasebuffer_handle(src_handle);
//     releasebuffer_handle(dst_handle);

//     return ret;
// }
