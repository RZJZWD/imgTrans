#include "capture_uvc.h"
#include "convert.h"
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// 全局资源变量
static int fd = -1;
static uint8_t *buffer = NULL;
static uint8_t *rgb_buffer = NULL;
static uint8_t *raw_buffer_copy = NULL;
static uint32_t raw_buffer_size = 0;
static struct v4l2_buffer buf;
static struct v4l2_format fmt;
static enum v4l2_buf_type type;
static enum capture_color color_format = CAP_NONE;

// 拷贝一份原始数据
static void copy_raw_data(void) {
    if (!buffer || buffer == MAP_FAILED || buf.bytesused == 0) {
        // 清空数据但不释放内存，避免频繁分配
        raw_buffer_size = 0;
        return;
    }

    // 没有分配缓冲区，或者
    // 当前缓冲区太小，重新分配
    if (!raw_buffer_copy || raw_buffer_size < buf.bytesused) {
        // 释放旧内存
        if (raw_buffer_copy) {
            free(raw_buffer_copy);
        }

        // 分配新内存
        raw_buffer_copy = malloc(buf.bytesused);
        if (!raw_buffer_copy) {
            printf("raw内存分配失败\n");
            raw_buffer_size = 0;
            return;
        }
        raw_buffer_size = buf.bytesused;
    }
    memcpy(raw_buffer_copy, buffer, buf.bytesused);
    raw_buffer_size = buf.bytesused;
}

int capture_uvc_save(int width, int height, const char *output_filename) {
    if (rgb_buffer) {
        if (color_format == CAP_YUYV) {
            yuyv_to_rgb(buffer, rgb_buffer, fmt.fmt.pix.width,
                        fmt.fmt.pix.height);
        } else if (color_format == CAP_JPEG) {
            if (jpeg_to_rgb(buffer, buf.bytesused, rgb_buffer,
                            fmt.fmt.pix.width, fmt.fmt.pix.height) != 0) {
                perror("JPEG解码失败");
            }
        }
    } else {
        printf("RGB缓冲区未分配\n");
        return -1;
    }

    // 10. 保存文件
    char filename[256];

    // 保存为PPM（方便查看）
    snprintf(filename, sizeof(filename), "%s.ppm", output_filename);
    save_ppm(filename, rgb_buffer, width, height);

    // 保存为原始RGB（用于显示程序）
    snprintf(filename, sizeof(filename), "%s.rgb", output_filename);
    save_rgb(filename, rgb_buffer, width, height);

    // 清理
    free(rgb_buffer);
    return 0;
}

int capture_uvc_init(int width, int height, enum capture_color color) {
    const char *device = "/dev/video0";

    // 错误处理：释放资源，
    // 这里设置临时变量，通过临时变量来决定是否释放资源
    int ret = 0;
    int fd_local = -1;
    void *buffer_local = MAP_FAILED;
    void *rgb_buffer_local = NULL;
    struct v4l2_format fmt_local;
    struct v4l2_buffer buf_local;
    enum v4l2_buf_type type_local;

    // 检查color参数
    if (color <= CAP_NONE || color >= CAP_NUMS) {
        return -1;
    }

    printf("打开摄像头: %s\n", device);
    printf("分辨率: %dx%d\n", width, height);

    // 1. 打开设备
    fd_local = open(device, O_RDWR);
    if (fd_local < 0) {
        perror("打开摄像头失败");
        ret = -1;
        goto cleanup;
    }

    // 2. 查询设备能力
    struct v4l2_capability cap;
    if (ioctl(fd_local, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("查询设备能力失败");
        ret = -1;
        goto cleanup;
    }

    printf("摄像头名称: %s\n", cap.card);
    printf("驱动: %s\n", cap.driver);

    // 3. 设置格式
    memset(&fmt_local, 0, sizeof(fmt_local));
    fmt_local.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt_local.fmt.pix.width = width;
    fmt_local.fmt.pix.height = height;
    if (color == CAP_YUYV) {
        fmt_local.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    } else if (color == CAP_JPEG) {
        fmt_local.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    }
    fmt_local.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd_local, VIDIOC_S_FMT, &fmt_local) < 0) {
        perror("设置格式失败");
        ret = -1;
        goto cleanup;
    }

    /******配置缓冲区*********/
    // 4. 请求缓冲区
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_local, VIDIOC_REQBUFS, &req) < 0) {
        perror("请求缓冲区失败");
        ret = -1;
        goto cleanup;
    }

    // 5. 映射缓冲区
    memset(&buf_local, 0, sizeof(buf_local));
    buf_local.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf_local.memory = V4L2_MEMORY_MMAP;
    buf_local.index = 0;

    if (ioctl(fd_local, VIDIOC_QUERYBUF, &buf_local) < 0) {
        perror("查询缓冲区失败");
        ret = -1;
        goto cleanup;
    }

    buffer_local = mmap(NULL, buf_local.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd_local, buf_local.m.offset);
    if (buffer_local == MAP_FAILED) {
        perror("映射缓冲区失败");
        ret = -1;
        goto cleanup;
    }

    printf("缓冲区大小: %d\n", buf_local.length);

    // 6. 入队缓冲区
    if (ioctl(fd_local, VIDIOC_QBUF, &buf_local) < 0) {
        perror("缓冲区入队失败");
        ret = -1;
        goto cleanup;
    }

    rgb_buffer_local = malloc(width * height * 3);
    if (!rgb_buffer_local) {
        printf("rgb内存分配失败\n");
        ret = -1;
        goto cleanup;
    }

    // 7. 开始捕获流
    type_local = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_local, VIDIOC_STREAMON, &type_local) < 0) {
        perror("开始流失败");
        ret = -1;
        goto cleanup;
    }

    // 所有步骤成功，赋值给全局变量
    fd = fd_local;
    buffer = buffer_local;
    buf = buf_local;
    fmt = fmt_local;
    type = type_local;
    rgb_buffer = rgb_buffer_local;
    color_format = color;

    printf("摄像头流已启动\n");
    return 0;

cleanup:
    // 按照资源分配的逆序释放
    if (type_local == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        ioctl(fd_local, VIDIOC_STREAMOFF, &type_local);
    }

    if (rgb_buffer_local) {
        free(rgb_buffer_local);
    }

    if (buffer_local != MAP_FAILED) {
        munmap(buffer_local, buf_local.length);
    }

    if (fd_local >= 0) {
        close(fd_local);
    }

    return ret;
}
int capture_uvc_captureImg(void) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        printf("等待帧超时或出错\n");
        return -1;
    }

    // 缓冲区出队
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("获取帧失败");
        return -1;
    }

    // 检查MJPEG数据有效性
    if (color_format == CAP_JPEG) {
        // 检查MJPEG数据是否有有效的起始和结束标记
        if (buf.bytesused < 100 || buffer[0] != 0xFF ||
            buffer[1] != 0xD8 || // SOI
            buffer[buf.bytesused - 2] != 0xFF ||
            buffer[buf.bytesused - 1] != 0xD9) { // EOI
            printf("警告: MJPEG数据格式无效或损坏\n");
            // 重新入队缓冲区继续捕获
            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                perror("缓冲区重新入队失败");
            }
            return -1;
        }
    }
    // printf("捕获到帧! 大小: %d\n", buf.bytesused);
    // 转换为RGB
    if (rgb_buffer) {
        if (color_format == CAP_YUYV) {
            yuyv_to_rgb(buffer, rgb_buffer, fmt.fmt.pix.width,
                        fmt.fmt.pix.height);
        } else if (color_format == CAP_JPEG) {
            if (jpeg_to_rgb(buffer, buf.bytesused, rgb_buffer,
                            fmt.fmt.pix.width, fmt.fmt.pix.height) != 0) {
                perror("JPEG解码失败");
            }
        }
    } else {
        printf("RGB缓冲区未分配\n");
        return -1;
    }
    copy_raw_data();

    // 重新将缓冲区入队以继续捕获
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("缓冲区重新入队失败");
        return -1;
    }
    return 0;
}

void capture_uvc_clean() {
    // 检查资源有效性后再释放
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }

    if (rgb_buffer) {
        free(rgb_buffer);
        rgb_buffer = NULL;
    }

    if (buffer != MAP_FAILED) {
        munmap(buffer, buf.length);
        buffer = MAP_FAILED;
    }

    if (fd >= 0) {
        close(fd);
        fd = -1;
    }

    printf("捕获结束!\n");
}

uint8_t *capture_uvc_getRGBbuffer(void) { return rgb_buffer; }

// uvc摄像头原始数据，在捕获下一帧前一直在
uint8_t *capture_getRawbuffer(uint32_t *raw_buf_size) {
    if (!raw_buffer_copy || raw_buffer_size == 0) {
        if (raw_buf_size)
            *raw_buf_size = 0;
        return NULL;
    }
    *raw_buf_size = raw_buffer_size;
    return raw_buffer_copy;
}