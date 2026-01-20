#include <fcntl.h>
#include <linux/videodev2.h>
#include <stddef.h> // 添加stddef.h
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// JPEG库头文件 - 在包含jpeglib.h之前包含stdio.h
#include <jpeglib.h>

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
// // 修复YUYV转RGB函数
// void yuyv_to_rgb(uint8_t *yuyv, uint8_t *rgb, int width, int height) {
//     for (int i = 0; i < height; i++) {
//         for (int j = 0; j < width; j += 2) {
//             int y0 = yuyv[(i * width + j) * 2];
//             int u = yuyv[(i * width + j) * 2 + 1];
//             int y1 = yuyv[(i * width + j) * 2 + 2];
//             int v = yuyv[(i * width + j) * 2 + 3];

//             // 正确的YUV->RGB转换公式 (BT.601标准)
//             int c0 = y0 - 16;
//             int c1 = y1 - 16;
//             int d = u - 128;
//             int e = v - 128;

//             // 第一个像素
//             int r0 = (298 * c0 + 409 * e + 128) >> 8;
//             int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
//             int b0 = (298 * c0 + 516 * d + 128) >> 8;

//             // 第二个像素
//             int r1 = (298 * c1 + 409 * e + 128) >> 8;
//             int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
//             int b1 = (298 * c1 + 516 * d + 128) >> 8;

//             // 限制范围并交换R/B通道 (BGR格式)
// #define CLAMP(x) (x < 0 ? 0 : (x > 255 ? 255 : x))
//             rgb[(i * width + j) * 3 + 0] = CLAMP(b0); // B
//             rgb[(i * width + j) * 3 + 1] = CLAMP(g0); // G
//             rgb[(i * width + j) * 3 + 2] = CLAMP(r0); // R

//             rgb[(i * width + j + 1) * 3 + 0] = CLAMP(b1); // B
//             rgb[(i * width + j + 1) * 3 + 1] = CLAMP(g1); // G
//             rgb[(i * width + j + 1) * 3 + 2] = CLAMP(r1); // R
//         }
//     }
// }

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

// RGB转JPEG保存
void save_jpeg(const char *filename, uint8_t *rgb, int width, int height,
               int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *outfile;
    JSAMPROW row_pointer[1];
    int row_stride;

    // 打开输出文件
    if ((outfile = fopen(filename, "wb")) == NULL) {
        fprintf(stderr, "无法打开JPEG文件: %s\n", filename);
        return;
    }

    // 初始化JPEG压缩对象
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, outfile);

    // 设置图像参数
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3; // RGB
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // 逐行写入数据
    row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb[cinfo.next_scanline * row_stride];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    // 完成压缩
    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);

    printf("已保存: %s (尺寸: %dx%d, 质量: %d%%)\n", filename, width, height,
           quality);
}

int main(int argc, char *argv[]) {
    const char *device = "/dev/video0";
    int width = 640;
    int height = 480;
    const char *output_base = "capture";
    int jpeg_quality = 85; // JPEG质量默认值

    // 解析参数
    if (argc > 1)
        device = argv[1];
    if (argc > 3) {
        width = atoi(argv[2]);
        height = atoi(argv[3]);
    }
    if (argc > 4) {
        jpeg_quality = atoi(argv[4]);
    }

    printf("打开摄像头: %s\n", device);
    printf("分辨率: %dx%d\n", width, height);
    printf("JPEG质量: %d%%\n", jpeg_quality);

    // 1. 打开设备
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("打开摄像头失败");
        return -1;
    }

    // 2. 查询设备能力
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("查询设备能力失败");
        close(fd);
        return -1;
    }

    printf("摄像头名称: %s\n", cap.card);
    printf("驱动: %s\n", cap.driver);

    // 3. 设置格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("设置格式失败");
        close(fd);
        return -1;
    }

    printf("实际格式: %c%c%c%c\n", fmt.fmt.pix.pixelformat & 0xFF,
           (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
           (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
           (fmt.fmt.pix.pixelformat >> 24) & 0xFF);
    printf("实际分辨率: %dx%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 4. 请求缓冲区
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("请求缓冲区失败");
        close(fd);
        return -1;
    }

    // 5. 映射缓冲区
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        perror("查询缓冲区失败");
        close(fd);
        return -1;
    }

    uint8_t *buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, buf.m.offset);
    if (buffer == MAP_FAILED) {
        perror("映射缓冲区失败");
        close(fd);
        return -1;
    }

    printf("缓冲区大小: %d\n", buf.length);

    // 6. 入队缓冲区
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("缓冲区入队失败");
        munmap(buffer, buf.length);
        close(fd);
        return -1;
    }

    // 7. 开始捕获
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("开始流失败");
        munmap(buffer, buf.length);
        close(fd);
        return -1;
    }

    // 8. 捕获一帧
    printf("等待帧...\n");
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        printf("等待帧超时或出错\n");
        goto cleanup;
    }

    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("获取帧失败");
        goto cleanup;
    }

    printf("捕获到帧! 大小: %d\n", buf.bytesused);

    // 9. 转换为RGB
    uint8_t *rgb_buffer = malloc(width * height * 3);
    if (!rgb_buffer) {
        printf("内存分配失败\n");
        goto cleanup;
    }

    yuyv_to_rgb(buffer, rgb_buffer, width, height);

    // 10. 保存文件
    char filename[256];

    // 保存为PPM（方便查看）
    snprintf(filename, sizeof(filename), "%s.ppm", output_base);
    save_ppm(filename, rgb_buffer, width, height);

    // 保存为原始RGB（用于显示程序）
    snprintf(filename, sizeof(filename), "%s.rgb", output_base);
    save_rgb(filename, rgb_buffer, width, height);

    // 保存为JPEG
    snprintf(filename, sizeof(filename), "%s.jpg", output_base);
    save_jpeg(filename, rgb_buffer, width, height, jpeg_quality);

    // 清理
    free(rgb_buffer);

cleanup:
    // 停止流
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    // 清理资源
    munmap(buffer, buf.length);
    close(fd);

    printf("捕获完成!\n");
    return 0;
}