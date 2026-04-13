#include "convert.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// jpeg解码
#include <dlfcn.h>
#include <turbojpeg.h>
// rga
#include <RgaApi.h>
#include <im2d.h>
// neno
#include <arm_neon.h>

/* ==================== 内部工具函数（静态） ==================== */
// 饱和裁剪到 [0, 255]
static inline uint8_t clamp(int x);

/* 通用 RGA 转换工具：接收 fd 和格式，执行一次转换 */
static int rga_convert_common(int src_fd, int dst_fd, int width, int height,
                              int src_fmt, int dst_fmt, int cvt_mode);

/* ==================== JPEG 解码等公开函数实现 ==================== */
int jpeg_get_version() {
    void *handle = dlopen("libturbojpeg.so", RTLD_LAZY);
    if (handle) {
        // 检查新版本 API 函数是否存在
        if (dlsym(handle, "tj3GetVersion")) {
            printf("检测到 TurboJPEG 3.x API\n");
        } else if (dlsym(handle, "tjInitDecompress")) {
            printf("检测到 TurboJPEG 2.x API\n");
        }
        dlclose(handle);
    }
}
int jpeg_to_yuv420p_turbo(uint8_t *jpeg_data, unsigned long jpeg_size,
                          uint8_t *yuv420_data, int width, int height) {
    tjhandle handle = tjInitDecompress();
    if (!handle) {
        fprintf(stderr, "tjInitDecompress failed\n");
        return -1;
    }

    int jpeg_width, jpeg_height, subsamp, colorspace;
    if (tjDecompressHeader3(handle, jpeg_data, jpeg_size, &jpeg_width,
                            &jpeg_height, &subsamp, &colorspace) < 0) {
        fprintf(stderr, "tjDecompressHeader3: %s\n", tjGetErrorStr2(handle));
        tjDestroy(handle);
        return -1;
    }
    if (jpeg_width != width || jpeg_height != height) {
        fprintf(stderr, "JPEG尺寸不匹配: 期望 %dx%d, 实际 %dx%d\n", width,
                height, jpeg_width, jpeg_height);
        tjDestroy(handle);
        return -1;
    }

    // 目标YUV420P的平面布局
    uint8_t *planes[3] = {
        yuv420_data,                                              // Y
        yuv420_data + width * height,                             // U
        yuv420_data + width * height + (width / 2) * (height / 2) // V
    };
    int strides[3] = {width, width / 2, width / 2}; // 每个平面的行字节数

    int flags = TJFLAG_FASTDCT;
    int ret = 0;

    if (subsamp == TJSAMP_420) {
        // 直接解码到目标平面（无需转换）
        if (tjDecompressToYUVPlanes(handle, jpeg_data, jpeg_size, planes, width,
                                    strides, height, flags) < 0) {
            fprintf(stderr, "tjDecompressToYUVPlanes: %s\n",
                    tjGetErrorStr2(handle));
            ret = -1;
        }
    } else if (subsamp == TJSAMP_422) {
        // 需要将422转换为420：先解码到临时平面（无填充布局），再垂直下采样
        // 临时缓冲区：Y平面 pitch = width，U/V平面 pitch = width/2（无填充）
        int y_size = width * height;
        int uv_size_422 = (width / 2) * height;
        uint8_t *tmp_y = malloc(y_size);
        uint8_t *tmp_u = malloc(uv_size_422);
        uint8_t *tmp_v = malloc(uv_size_422);
        if (!tmp_y || !tmp_u || !tmp_v) {
            fprintf(stderr, "malloc failed\n");
            ret = -1;
            goto cleanup_422;
        }

        uint8_t *tmp_planes[3] = {tmp_y, tmp_u, tmp_v};
        int tmp_strides[3] = {width, width / 2, width / 2}; // 无填充布局

        if (tjDecompressToYUVPlanes(handle, jpeg_data, jpeg_size, tmp_planes,
                                    width, tmp_strides, height, flags) < 0) {
            fprintf(stderr, "tjDecompressToYUVPlanes: %s\n",
                    tjGetErrorStr2(handle));
            ret = -1;
            goto cleanup_422;
        }

        // 复制Y平面（直接拷贝，尺寸相同）
        memcpy(planes[0], tmp_y, y_size);

        // 垂直平均U和V (422 → 420)
        uint8_t *dst_u = planes[1];
        uint8_t *dst_v = planes[2];
        int uv_width = width / 2;
        for (int row = 0; row < height; row += 2) {
            for (int col = 0; col < uv_width; col++) {
                int src_idx =
                    row * uv_width + col; // 临时U/V平面每行 uv_width 字节
                int dst_idx = (row / 2) * uv_width + col;
                dst_u[dst_idx] =
                    (tmp_u[src_idx] + tmp_u[src_idx + uv_width]) >> 1;
                dst_v[dst_idx] =
                    (tmp_v[src_idx] + tmp_v[src_idx + uv_width]) >> 1;
            }
        }

    cleanup_422:
        free(tmp_y);
        free(tmp_u);
        free(tmp_v);
    } else if (subsamp == TJSAMP_444) {
        // 需要将444转换为420：先解码到临时平面（无填充布局），再水平+垂直下采样
        int y_size = width * height;
        int uv_size_444 = width * height; // 444的U/V平面大小
        uint8_t *tmp_y = malloc(y_size);
        uint8_t *tmp_u = malloc(uv_size_444);
        uint8_t *tmp_v = malloc(uv_size_444);
        if (!tmp_y || !tmp_u || !tmp_v) {
            fprintf(stderr, "malloc failed\n");
            ret = -1;
            goto cleanup_444;
        }

        uint8_t *tmp_planes[3] = {tmp_y, tmp_u, tmp_v};
        int tmp_strides[3] = {width, width, width}; // 无填充，每行 width 字节

        if (tjDecompressToYUVPlanes(handle, jpeg_data, jpeg_size, tmp_planes,
                                    width, tmp_strides, height, flags) < 0) {
            fprintf(stderr, "tjDecompressToYUVPlanes: %s\n",
                    tjGetErrorStr2(handle));
            ret = -1;
            goto cleanup_444;
        }

        // 复制Y平面
        memcpy(planes[0], tmp_y, y_size);

        // 水平+垂直下采样U和V (444 → 420)
        uint8_t *dst_u = planes[1];
        uint8_t *dst_v = planes[2];
        int uv_width = width / 2;
        for (int row = 0; row < height; row += 2) {
            for (int col = 0; col < uv_width; col++) {
                // 源U/V平面每行 width 字节，需要平均2x2块
                int src_idx00 = row * width + col * 2;
                int src_idx01 = src_idx00 + 1;
                int src_idx10 = (row + 1) * width + col * 2;
                int src_idx11 = src_idx10 + 1;
                int dst_idx = (row / 2) * uv_width + col;

                dst_u[dst_idx] = (tmp_u[src_idx00] + tmp_u[src_idx01] +
                                  tmp_u[src_idx10] + tmp_u[src_idx11] + 2) >>
                                 2;
                dst_v[dst_idx] = (tmp_v[src_idx00] + tmp_v[src_idx01] +
                                  tmp_v[src_idx10] + tmp_v[src_idx11] + 2) >>
                                 2;
            }
        }

    cleanup_444:
        free(tmp_y);
        free(tmp_u);
        free(tmp_v);
    } else {
        fprintf(stderr, "不支持的JPEG采样格式: %d\n", subsamp);
        ret = -1;
    }

    tjDestroy(handle);
    return ret;
}
int jpeg_to_rgb888_turbo(uint8_t *jpeg_data, unsigned long jpeg_size,
                         uint8_t *bgr_data, int width, int height) {
    tjhandle handle = tjInitDecompress();
    if (!handle)
        return -1;

    int jpeg_width, jpeg_height, subsamp, colorspace;
    if (tjDecompressHeader3(handle, jpeg_data, jpeg_size, &jpeg_width,
                            &jpeg_height, &subsamp, &colorspace) < 0) {
        tjDestroy(handle);
        return -1;
    }

    // 验证尺寸
    if (jpeg_width != width || jpeg_height != height) {
        fprintf(stderr, "JPEG尺寸不匹配: 期望 %dx%d, 实际 %dx%d\n", width,
                height, jpeg_width, jpeg_height);
        tjDestroy(handle);
        return -1;
    }

    // 设置像素格式为 BGR（适用于 LVGL 小端显示）
    int pixel_format = TJPF_BGR;
    // 使用快速 DCT 算法，牺牲少许画质换取速度
    int flags = TJFLAG_FASTDCT;

    if (tjDecompress2(handle, jpeg_data, jpeg_size, bgr_data, width, 0, height,
                      pixel_format, flags) < 0) {
        tjDestroy(handle);
        return -1;
    }

    tjDestroy(handle);
    return 0;
}
/* ==================== 5. 统一转换入口实现 ==================== */

int yuyv422_to_yuv420p(dmabuf_buffer_t *src_buf, dmabuf_buffer_t *dst_buf,
                       int width, int height, convert_mode_t mode) {
    /* 从 dmabuf_buffer_t 获取数据指针，根据 mode 调用对应的静态底层函数 */
    if (!src_buf || !dst_buf) {
        fprintf(stderr, "Invalid buffer pointer\n");
        return -1;
    }

    switch (mode) {
    case CONVERT_MODE_CPU_SCALAR:
        return yuyv422_to_yuv420p_sw(dmabuf_get_data_ptr(src_buf),
                                     dmabuf_get_data_ptr(dst_buf), width,
                                     height);
    case CONVERT_MODE_CPU_SIMD:
        return yuyv422_to_yuv420p_neno(dmabuf_get_data_ptr(src_buf),
                                       dmabuf_get_data_ptr(dst_buf), width,
                                       height);
    case CONVERT_MODE_RGA:
        return yuyv422_to_yuv420p_rga(dmabuf_get_fd(src_buf),
                                      dmabuf_get_fd(dst_buf), width, height);
    default:
        fprintf(stderr, "Unsupported convert mode\n");
        return -1;
    }
}

int yuv420p_to_rgb888(dmabuf_buffer_t *src_buf, dmabuf_buffer_t *dst_buf,
                      int width, int height, convert_mode_t mode) {
    if (!src_buf || !dst_buf) {
        fprintf(stderr, "Invalid buffer pointer\n");
        return -1;
    }

    switch (mode) {
    case CONVERT_MODE_CPU_SCALAR:
        return yuv420p_to_rgb888_sw(dmabuf_get_data_ptr(src_buf),
                                    dmabuf_get_data_ptr(dst_buf), width,
                                    height);
    case CONVERT_MODE_CPU_SIMD:
        return yuv420p_to_rgb888_neno(dmabuf_get_data_ptr(src_buf),
                                      dmabuf_get_data_ptr(dst_buf), width,
                                      height);
    case CONVERT_MODE_RGA:
        return yuv420p_to_rgb888_rga(dmabuf_get_fd(src_buf),
                                     dmabuf_get_fd(dst_buf), width, height);
    default:
        fprintf(stderr, "Unsupported convert mode\n");
        return -1;
    }
}

int yuv420p_to_bgr888(dmabuf_buffer_t *src_buf, dmabuf_buffer_t *dst_buf,
                      int width, int height, convert_mode_t mode) {
    if (!src_buf || !dst_buf) {
        fprintf(stderr, "Invalid buffer pointer\n");
        return -1;
    }

    switch (mode) {
    case CONVERT_MODE_CPU_SCALAR:
        return yuv420p_to_bgr888_sw(dmabuf_get_data_ptr(src_buf),
                                    dmabuf_get_data_ptr(dst_buf), width,
                                    height);
    case CONVERT_MODE_CPU_SIMD:
        return yuv420p_to_bgr888_neno(dmabuf_get_data_ptr(src_buf),
                                      dmabuf_get_data_ptr(dst_buf), width,
                                      height);
    case CONVERT_MODE_RGA:
        return yuv420p_to_bgr888_rga(dmabuf_get_fd(src_buf),
                                     dmabuf_get_fd(dst_buf), width, height);
    default:
        fprintf(stderr, "Unsupported convert mode\n");
        return -1;
    }
}

/* ==================== 6. 静态函数具体实现 ==================== */
/* 所有前面声明的 static 函数的具体代码放在此处 */

/* ---- 工具函数 ---- */
static inline uint8_t clamp(int x) {
    if (x < 0)
        return 0;
    if (x > 255)
        return 255;
    return (uint8_t)x;
}

/**
 * @brief RGA 格式转换通用实现（使用 DMA-BUF fd）
 * @param src_fd    源 dmabuf 文件描述符
 * @param dst_fd    目标 dmabuf 文件描述符
 * @param width     图像宽度
 * @param height    图像高度
 * @param src_fmt   源 RGA 格式
 * @param dst_fmt   目标 RGA 格式
 * @param cvt_mode  imcvtcolor 模式
 * @return 成功返回 0，失败返回 -1
 */
static int rga_convert_common(int src_fd, int dst_fd, int width, int height,
                              int src_fmt, int dst_fmt, int cvt_mode) {
    rga_buffer_handle_t src_handle = 0, dst_handle = 0;
    im_handle_param_t src_param = {0}, dst_param = {0};
    rga_buffer_t src_img, dst_img;
    int ret = -1;

    // 统一检查 fd 有效性
    if (src_fd < 0 || dst_fd < 0) {
        fprintf(stderr, "RGA conversion failed: invalid fd (src=%d, dst=%d)\n",
                src_fd, dst_fd);
        return -1;
    }

    src_param.width = dst_param.width = width;
    src_param.height = dst_param.height = height;
    src_param.format = src_fmt;
    dst_param.format = dst_fmt;

    src_handle = importbuffer_fd(src_fd, &src_param);
    dst_handle = importbuffer_fd(dst_fd, &dst_param);
    if (src_handle == 0 || dst_handle == 0) {
        fprintf(stderr, "RGA importbuffer_fd failed\n");
        goto out;
    }

    src_img = wrapbuffer_handle(src_handle, width, height, src_fmt);
    dst_img = wrapbuffer_handle(dst_handle, width, height, dst_fmt);

    if (imcvtcolor(src_img, dst_img, src_fmt, dst_fmt, cvt_mode) !=
        IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA imcvtcolor failed\n");
        ret = -1;
    } else {
        ret = 0;
    }

out:
    if (src_handle)
        releasebuffer_handle(src_handle);
    if (dst_handle)
        releasebuffer_handle(dst_handle);
    return ret;
}

/* ---- 纯软件标量版本实现 ---- */
int yuyv422_to_yuv420p_sw(const uint8_t *yuyv, uint8_t *yuv420p, int width,
                          int height) {
    if (!yuyv || !yuv420p || width <= 0 || height <= 0 || width % 2 != 0 ||
        height % 2 != 0) {
        fprintf(stderr, "yuyv_to_yuv420p: invalid parameters\n");
        return -1;
    }
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
    return 0;
}
// YUV420P 转 RGB888 (BT.601 limited range)
int yuv420p_to_rgb888_sw(void *yuv_data, void *rgb_data, int width,
                         int height) {
    if (!yuv_data || !rgb_data || width <= 0 || height <= 0 || width % 2 != 0 ||
        height % 2 != 0) {
        fprintf(stderr, "Invalid parameters or dimensions not multiple of 2\n");
        return -1;
    }

    uint8_t *y_plane = (uint8_t *)yuv_data;
    uint8_t *u_plane = y_plane + width * height;
    uint8_t *v_plane = u_plane + (width * height) / 4;
    uint8_t *rgb = (uint8_t *)rgb_data;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int y_idx = j * width + i;
            int uv_idx = (j / 2) * (width / 2) + (i / 2);

            int y = y_plane[y_idx];
            int u = u_plane[uv_idx];
            int v = v_plane[uv_idx];

            // 将 Y 从 limited range 扩展到 full range
            // 或者直接使用公式（此处直接使用） BT.601 整数公式
            int r = y + ((359 * (v - 128)) >> 8);
            int g = y - ((88 * (u - 128) + 183 * (v - 128)) >> 8);
            int b = y + ((454 * (u - 128)) >> 8);

            int rgb_idx = y_idx * 3;
            rgb[rgb_idx] = clamp(r);
            rgb[rgb_idx + 1] = clamp(g);
            rgb[rgb_idx + 2] = clamp(b);
        }
    }

    return 0;
}
int yuv420p_to_bgr888_sw(void *yuv_data, void *bgr_data, int width,
                         int height) {
    if (!yuv_data || !bgr_data || width <= 0 || height <= 0 || width % 2 != 0 ||
        height % 2 != 0) {
        fprintf(stderr, "Invalid parameters or dimensions not multiple of 2\n");
        return -1;
    }

    uint8_t *y_plane = (uint8_t *)yuv_data;
    uint8_t *u_plane = y_plane + width * height;
    uint8_t *v_plane = u_plane + (width * height) / 4;
    uint8_t *rgb = (uint8_t *)bgr_data;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int y_idx = j * width + i;
            int uv_idx = (j / 2) * (width / 2) + (i / 2);

            int y = y_plane[y_idx];
            int u = u_plane[uv_idx];
            int v = v_plane[uv_idx];

            // 将 Y 从 limited range 扩展到 full range
            // 或者直接使用公式（此处直接使用） BT.601 整数公式
            int b = y + ((359 * (v - 128)) >> 8);
            int g = y - ((88 * (u - 128) + 183 * (v - 128)) >> 8);
            int r = y + ((454 * (u - 128)) >> 8);

            int rgb_idx = y_idx * 3;
            rgb[rgb_idx] = clamp(r);
            rgb[rgb_idx + 1] = clamp(g);
            rgb[rgb_idx + 2] = clamp(b);
        }
    }

    return 0;
}

/* ---- SIMD 加速版本实现 ---- */
int yuyv422_to_yuv420p_neno(uint8_t *yuyv, uint8_t *yuv420p, int width,
                            int height) {
    if (!yuyv || !yuv420p || width <= 0 || height <= 0 || width % 2 != 0 ||
        height % 2 != 0) {
        fprintf(stderr, "yuyv_to_yuv420p: invalid parameters\n");
        return -1;
    }
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);
    uint8_t *y_plane = yuv420p;
    uint8_t *u_plane = yuv420p + y_size;
    uint8_t *v_plane = yuv420p + y_size + uv_size;

    int y_stride = width;
    int uv_stride = width / 2;
    int yuyv_stride = width * 2;

    // 一次处理奇偶两行
    for (int h = 0; h < height; h += 2) {
        uint8_t *yuyv_row0 = yuyv + h * yuyv_stride;
        uint8_t *yuyv_row1 = yuyv + (h + 1) * yuyv_stride;

        uint8_t *y_row0 = y_plane + h * y_stride;
        uint8_t *y_row1 = y_plane + (h + 1) * y_stride;

        uint8_t *u_row = u_plane + (h / 2) * uv_stride;
        uint8_t *v_row = v_plane + (h / 2) * uv_stride;

        // 内层每次处理16个像素（8个yuyv像素）32字节
        for (int w = 0; w < width; w += 16) {
            int yuyv_offset = w * 2; // 每个像素两字节
            uint8_t *src0 = yuyv_row0 + yuyv_offset;
            uint8_t *src1 = yuyv_row1 + yuyv_offset;

            // 加载偶数行的YUYV数据（8个宏像素 -> 16像素）
            // vld4_u8 从src0加载32字节，返回4个uint8x8_t，分别对应：
            // val[0]: 所有偶数Y (Y0,Y2,Y4...)
            // val[1]: 所有U (U0,U1,U2...)
            // val[2]: 所有奇数Y (Y1,Y3,Y5...)
            // val[3]: 所有V (V0,V1,V2...)
            uint8x8x4_t yuyv0 = vld4_u8(src0);
            uint8x8_t y0_even = yuyv0.val[0];
            uint8x8_t u_even = yuyv0.val[1];
            uint8x8_t y1_even = yuyv0.val[2];
            uint8x8_t v_even = yuyv0.val[3];

            // 加载奇数行的YUYV数据
            uint8x8x4_t yuyv1 = vld4_u8(src1);
            uint8x8_t y0_odd = yuyv1.val[0];
            uint8x8_t u_odd = yuyv1.val[1];
            uint8x8_t y1_odd = yuyv1.val[2];
            uint8x8_t v_odd = yuyv1.val[3];

            // 处理y，偶数行
            uint8x8x2_t y_zip_even = vzip_u8(y0_even, y1_even);
            uint8x16_t y_all_even =
                vcombine_u8(y_zip_even.val[0], y_zip_even.val[1]);
            vst1q_u8(y_row0 + w, y_all_even);

            // 处理y，奇数行
            uint8x8x2_t y_zip_odd = vzip_u8(y0_odd, y1_odd);
            uint8x16_t y_all_odd =
                vcombine_u8(y_zip_odd.val[0], y_zip_odd.val[1]);
            vst1q_u8(y_row1 + w, y_all_odd);

            // 处理uv，垂直平均
            uint8x8_t avg_u = vrhadd_u8(u_even, u_odd);
            uint8x8_t avg_v = vrhadd_u8(v_even, v_odd);

            // 存储到uv平面
            int uv_col = w / 2;
            vst1_u8(u_row + uv_col, avg_u);
            vst1_u8(v_row + uv_col, avg_v);
        }
    }
    return 0;
}
int yuv420p_to_rgb888_neno(void *yuv_data, void *rgb_data, int width,
                           int height) {
    if (!yuv_data || !rgb_data || width <= 0 || height <= 0 || width % 2 != 0 ||
        height % 2 != 0) {
        fprintf(stderr, "Invalid parameters or dimensions not multiple of 2\n");
        return -1;
    }

    uint8_t *y_plane = (uint8_t *)yuv_data;
    uint8_t *u_plane = y_plane + width * height;       // U平面起始
    uint8_t *v_plane = u_plane + (width * height) / 4; // V平面起始
    uint8_t *rgb = (uint8_t *)rgb_data;

    int y_stride = width;      // Y平面行跨度（字节）
    int uv_stride = width / 2; // U/V平面行跨度

    for (int h = 0; h < height; h++) {
        int uv_row = h / 2; // 当前行对应的UV平面行索引，每两行y共用一行uv

        uint8_t *y_row = y_plane + h * y_stride;
        uint8_t *u_row = u_plane + uv_row * uv_stride;
        uint8_t *v_row = v_plane + uv_row * uv_stride;
        uint8_t *rgb_row = rgb + h * width * 3;

        // 内层使用neno加速，一次处理8个像素
        for (int w = 0; w < width; w += 8) {
            int uv_offset = w / 2; // 当前8像素对应的U/V起始索引

            uint8_t *y_ptr = y_row + w;
            uint8_t *u_ptr = u_row + uv_offset;
            uint8_t *v_ptr = v_row + uv_offset;
            uint8_t *rgb_ptr = rgb_row + w * 3;

            // neno处理
            // 1.加载数据
            uint8x8_t y8 = vld1_u8(y_ptr); // 8个y值
            uint8x8_t u4 = vld1_u8(u_ptr); // 4个u值(低四个)
            uint8x8_t v4 = vld1_u8(v_ptr); // 4个v值

            // 2.将uv扩展为16位，然后复制成与y对应的8个值
            // 扩展8个字节，低4个16位是U0~U3，高4个是垃圾
            uint16x8_t u16_all = vmovl_u8(u4);
            uint16x8_t v16_all = vmovl_u8(v4); // 同理

            uint16x4_t u16_low =
                vget_low_u16(u16_all); // 提取有效的4个U值 {u0,u1,u2,u3}
            uint16x4_t v16_low =
                vget_low_u16(v16_all); // 提取有效的4个V值 {v0,v1,v2,v3}

            // 3. 构建与8个Y对应的U/V向量：每个U/V重复两次
            // 使用vzip_u16生成 {u0,u0,u1,u1} 和 {u2,u2,u3,u3}，然后合并
            uint16x4x2_t u_zip = vzip_u16(u16_low, u16_low);
            uint16x4x2_t v_zip = vzip_u16(v16_low, v16_low);
            uint16x8_t u16 = vcombine_u16(
                u_zip.val[0], u_zip.val[1]); // {u0,u0,u1,u1,u2,u2,u3,u3}
            uint16x8_t v16 = vcombine_u16(
                v_zip.val[0], v_zip.val[1]); // {v0,v0,v1,v1,v2,v2,v3,v3}

            // 4. 将Y扩展到16位
            uint16x8_t y16 = vmovl_u8(y8);

            // 5.计算差值，y-16 u-128,v-128
            int16x8_t c =
                vreinterpretq_s16_u16(vsubq_u16(y16, vdupq_n_u16(16)));
            int16x8_t d =
                vreinterpretq_s16_u16(vsubq_u16(u16, vdupq_n_u16(128)));
            int16x8_t e =
                vreinterpretq_s16_u16(vsubq_u16(v16, vdupq_n_u16(128)));

            // 6. 准备系数（16位有符号）
            int16x8_t coeff_298 = vdupq_n_s16(298);
            int16x8_t coeff_409 = vdupq_n_s16(409);
            int16x8_t coeff_100 = vdupq_n_s16(100);
            int16x8_t coeff_208 = vdupq_n_s16(208);
            int16x8_t coeff_516 = vdupq_n_s16(516);
            int32x4_t round = vdupq_n_s32(128); // 舍入常数

            // 7.计算R=（298*c + 409*e +128)>>8
            int32x4_t r_low =
                vmull_s16(vget_low_s16(c), vget_low_s16(coeff_298));
            int32x4_t r_high =
                vmull_s16(vget_high_s16(c), vget_high_s16(coeff_298));

            r_low = vmlal_s16(r_low, vget_low_s16(e), vget_low_s16(coeff_409));
            r_high =
                vmlal_s16(r_high, vget_high_s16(e), vget_high_s16(coeff_409));

            r_low = vaddq_s32(r_low, round);
            r_high = vaddq_s32(r_high, round);
            int16x8_t r16 =
                vcombine_s16(vqshrn_n_s32(r_low, 8), vqshrn_n_s32(r_high, 8));

            // 8. 计算G = (298*c - 100*d - 208*e + 128) >> 8
            int32x4_t g_low =
                vmull_s16(vget_low_s16(c), vget_low_s16(coeff_298));
            int32x4_t g_high =
                vmull_s16(vget_high_s16(c), vget_high_s16(coeff_298));

            g_low = vmlsl_s16(g_low, vget_low_s16(d), vget_low_s16(coeff_100));
            g_high =
                vmlsl_s16(g_high, vget_low_s16(d), vget_low_s16(coeff_100));

            g_low = vmlsl_s16(g_low, vget_low_s16(e), vget_low_s16(coeff_208));
            g_high =
                vmlsl_s16(g_high, vget_low_s16(e), vget_low_s16(coeff_208));

            g_low = vaddq_s32(g_low, round);
            g_high = vaddq_s32(g_high, round);
            int16x8_t g16 =
                vcombine_s16(vqshrn_n_s32(g_low, 8), vqshrn_n_s32(g_high, 8));

            // 9. 计算B = (298*c + 516*d + 128) >> 8
            int32x4_t b_low =
                vmull_s16(vget_low_s16(c), vget_low_s16(coeff_298));
            int32x4_t b_high =
                vmull_s16(vget_high_s16(c), vget_high_s16(coeff_298));

            b_low = vmlal_s16(b_low, vget_low_s16(d), vget_low_s16(coeff_516));
            b_high =
                vmlal_s16(b_high, vget_high_s16(d), vget_high_s16(coeff_516));

            b_low = vaddq_s32(b_low, round);
            b_high = vaddq_s32(b_high, round);
            int16x8_t b16 =
                vcombine_s16(vqshrn_n_s32(b_low, 8), vqshrn_n_s32(b_high, 8));

            // 10. 将16位结果饱和到8位（0~255）
            uint8x8_t r8 = vqmovun_s16(r16);
            uint8x8_t g8 = vqmovun_s16(g16);
            uint8x8_t b8 = vqmovun_s16(b16);

            // 11. 交错存储为RGB888格式
            uint8x8x3_t rgb;
            rgb.val[0] = r8;
            rgb.val[1] = g8;
            rgb.val[2] = b8;
            vst3_u8(rgb_ptr, rgb);
        }
    }
    return 0;
}
int yuv420p_to_bgr888_neno(void *yuv_data, void *bgr_data, int width,
                           int height) {
    if (!yuv_data || !bgr_data || width <= 0 || height <= 0 || width % 2 != 0 ||
        height % 2 != 0) {
        fprintf(stderr, "Invalid parameters or dimensions not multiple of 2\n");
        return -1;
    }

    uint8_t *y_plane = (uint8_t *)yuv_data;
    uint8_t *u_plane = y_plane + width * height;       // U平面起始
    uint8_t *v_plane = u_plane + (width * height) / 4; // V平面起始
    uint8_t *bgr = (uint8_t *)bgr_data;

    int y_stride = width;      // Y平面行跨度（字节）
    int uv_stride = width / 2; // U/V平面行跨度

    for (int h = 0; h < height; h++) {
        int uv_row = h / 2; // 当前行对应的UV平面行索引，每两行y共用一行uv

        uint8_t *y_row = y_plane + h * y_stride;
        uint8_t *u_row = u_plane + uv_row * uv_stride;
        uint8_t *v_row = v_plane + uv_row * uv_stride;
        uint8_t *bgr_row = bgr + h * width * 3;

        // 内层使用neno加速，一次处理8个像素
        for (int w = 0; w < width; w += 8) {
            int uv_offset = w / 2; // 当前8像素对应的U/V起始索引

            uint8_t *y_ptr = y_row + w;
            uint8_t *u_ptr = u_row + uv_offset;
            uint8_t *v_ptr = v_row + uv_offset;
            uint8_t *bgr_ptr = bgr_row + w * 3;

            // neno处理
            // 1.加载数据
            uint8x8_t y8 = vld1_u8(y_ptr); // 8个y值
            uint8x8_t u4 = vld1_u8(u_ptr); // 4个u值(低四个)
            uint8x8_t v4 = vld1_u8(v_ptr); // 4个v值

            // 2.将uv扩展为16位，然后复制成与y对应的8个值
            uint16x8_t u16_all =
                vmovl_u8(u4); // 扩展8个字节，低4个16位是U0~U3，高4个是垃圾
            uint16x8_t v16_all = vmovl_u8(v4); // 同理
            uint16x4_t u16_low =
                vget_low_u16(u16_all); // 提取有效的4个U值 {u0,u1,u2,u3}
            uint16x4_t v16_low =
                vget_low_u16(v16_all); // 提取有效的4个V值 {v0,v1,v2,v3}

            // 3. 构建与8个Y对应的U/V向量：每个U/V重复两次
            // 使用vzip_u16生成 {u0,u0,u1,u1} 和 {u2,u2,u3,u3}，然后合并
            uint16x4x2_t u_zip = vzip_u16(u16_low, u16_low);
            uint16x4x2_t v_zip = vzip_u16(v16_low, v16_low);
            uint16x8_t u16 = vcombine_u16(
                u_zip.val[0], u_zip.val[1]); // {u0,u0,u1,u1,u2,u2,u3,u3}
            uint16x8_t v16 = vcombine_u16(
                v_zip.val[0], v_zip.val[1]); // {v0,v0,v1,v1,v2,v2,v3,v3}

            // 4. 将Y扩展到16位
            uint16x8_t y16 = vmovl_u8(y8);

            // 5.计算差值，y-16 u-128,v-128
            int16x8_t c =
                vreinterpretq_s16_u16(vsubq_u16(y16, vdupq_n_u16(16)));
            int16x8_t d =
                vreinterpretq_s16_u16(vsubq_u16(u16, vdupq_n_u16(128)));
            int16x8_t e =
                vreinterpretq_s16_u16(vsubq_u16(v16, vdupq_n_u16(128)));

            // 6. 准备系数（16位有符号）
            int16x8_t coeff_298 = vdupq_n_s16(298);
            int16x8_t coeff_409 = vdupq_n_s16(409);
            int16x8_t coeff_100 = vdupq_n_s16(100);
            int16x8_t coeff_208 = vdupq_n_s16(208);
            int16x8_t coeff_516 = vdupq_n_s16(516);
            int32x4_t round = vdupq_n_s32(128); // 舍入常数

            // 7.计算R=（298*c + 409*e +128)>>8
            int32x4_t r_low =
                vmull_s16(vget_low_s16(c), vget_low_s16(coeff_298));
            int32x4_t r_high =
                vmull_s16(vget_high_s16(c), vget_high_s16(coeff_298));

            r_low = vmlal_s16(r_low, vget_low_s16(e), vget_low_s16(coeff_409));
            r_high =
                vmlal_s16(r_high, vget_high_s16(e), vget_high_s16(coeff_409));

            r_low = vaddq_s32(r_low, round);
            r_high = vaddq_s32(r_high, round);
            int16x8_t r16 =
                vcombine_s16(vqshrn_n_s32(r_low, 8), vqshrn_n_s32(r_high, 8));

            // 8. 计算G = (298*c - 100*d - 208*e + 128) >> 8
            int32x4_t g_low =
                vmull_s16(vget_low_s16(c), vget_low_s16(coeff_298));
            int32x4_t g_high =
                vmull_s16(vget_high_s16(c), vget_high_s16(coeff_298));

            g_low = vmlsl_s16(g_low, vget_low_s16(d), vget_low_s16(coeff_100));
            g_high =
                vmlsl_s16(g_high, vget_low_s16(d), vget_low_s16(coeff_100));

            g_low = vmlsl_s16(g_low, vget_low_s16(e), vget_low_s16(coeff_208));
            g_high =
                vmlsl_s16(g_high, vget_low_s16(e), vget_low_s16(coeff_208));

            g_low = vaddq_s32(g_low, round);
            g_high = vaddq_s32(g_high, round);
            int16x8_t g16 =
                vcombine_s16(vqshrn_n_s32(g_low, 8), vqshrn_n_s32(g_high, 8));

            // 9. 计算B = (298*c + 516*d + 128) >> 8
            int32x4_t b_low =
                vmull_s16(vget_low_s16(c), vget_low_s16(coeff_298));
            int32x4_t b_high =
                vmull_s16(vget_high_s16(c), vget_high_s16(coeff_298));

            b_low = vmlal_s16(b_low, vget_low_s16(d), vget_low_s16(coeff_516));
            b_high =
                vmlal_s16(b_high, vget_high_s16(d), vget_high_s16(coeff_516));

            b_low = vaddq_s32(b_low, round);
            b_high = vaddq_s32(b_high, round);
            int16x8_t b16 =
                vcombine_s16(vqshrn_n_s32(b_low, 8), vqshrn_n_s32(b_high, 8));

            // 10. 将16位结果饱和到8位（0~255）
            uint8x8_t r8 = vqmovun_s16(r16);
            uint8x8_t g8 = vqmovun_s16(g16);
            uint8x8_t b8 = vqmovun_s16(b16);

            // 11. 交错存储为BGR888格式
            uint8x8x3_t bgr;
            bgr.val[0] = b8;
            bgr.val[1] = g8;
            bgr.val[2] = r8;
            vst3_u8(bgr_ptr, bgr);
        }
    }
    return 0;
}

/* ---- RGA 硬件加速版本实现 ---- */
int yuyv422_to_yuv420p_rga(int src_fd, int dst_fd, int width, int height) {
    return rga_convert_common(src_fd, dst_fd, width, height, RK_FORMAT_YUYV_422,
                              RK_FORMAT_YCbCr_420_P, IM_YUV_TO_RGB_BT601_LIMIT);
}

int yuv420p_to_rgb888_rga(int src_fd, int dst_fd, int width, int height) {
    return rga_convert_common(src_fd, dst_fd, width, height,
                              RK_FORMAT_YCbCr_420_P, RK_FORMAT_RGB_888,
                              IM_YUV_TO_RGB_BT601_LIMIT);
}

int yuv420p_to_bgr888_rga(int src_fd, int dst_fd, int width, int height) {
    return rga_convert_common(src_fd, dst_fd, width, height,
                              RK_FORMAT_YCbCr_420_P, RK_FORMAT_BGR_888,
                              IM_YUV_TO_RGB_BT601_LIMIT);
}