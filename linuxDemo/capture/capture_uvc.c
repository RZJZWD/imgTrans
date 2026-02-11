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
static int v4l2_fd = -1;
static struct v4l2_format fmt;
static enum v4l2_buf_type type;
static enum capture_color color_format = CAP_NONE;
static int frames_local = 0;
#if (USE_MALLOC)
static struct v4l2_buffer buf;
static uint8_t *buffer = NULL;
static uint8_t *rgb_buffer = NULL;
static uint8_t *raw_buffer_copy = NULL;
static uint32_t raw_buffer_size = 0;
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

int capture_uvc_init(int width, int height, enum capture_color color) {
    const char *device = "/dev/video0";

    // 错误处理：释放资源，
    // 这里设置临时变量，通过临时变量来决定是否释放资源
    int ret = 0;
    int v4l2_fd_local = -1;
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
    v4l2_fd_local = open(device, O_RDWR);
    if (v4l2_fd_local < 0) {
        fprintf(stderr, "打开摄像头失败");
        ret = -1;
        goto cleanup;
    }

    // 2. 查询设备能力
    struct v4l2_capability cap;
    if (ioctl(v4l2_fd_local, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "查询设备能力失败");
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

    if (ioctl(v4l2_fd_local, VIDIOC_S_FMT, &fmt_local) < 0) {
        fprintf(stderr, "设置格式失败");
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

    if (ioctl(v4l2_fd_local, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "请求缓冲区失败");
        ret = -1;
        goto cleanup;
    }

    // 5. 映射缓冲区
    memset(&buf_local, 0, sizeof(buf_local));
    buf_local.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf_local.memory = V4L2_MEMORY_MMAP;
    buf_local.index = 0;

    if (ioctl(v4l2_fd_local, VIDIOC_QUERYBUF, &buf_local) < 0) {
        fprintf(stderr, "查询缓冲区失败");
        ret = -1;
        goto cleanup;
    }

    buffer_local = mmap(NULL, buf_local.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, v4l2_fd_local, buf_local.m.offset);
    if (buffer_local == MAP_FAILED) {
        fprintf(stderr, "映射缓冲区失败");
        ret = -1;
        goto cleanup;
    }

    printf("缓冲区大小: %d\n", buf_local.length);

    // 6. 入队缓冲区
    if (ioctl(v4l2_fd_local, VIDIOC_QBUF, &buf_local) < 0) {
        fprintf(stderr, "缓冲区入队失败");
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
    if (ioctl(v4l2_fd_local, VIDIOC_STREAMON, &type_local) < 0) {
        fprintf(stderr, "开始流失败");
        ret = -1;
        goto cleanup;
    }

    // 所有步骤成功，赋值给全局变量
    v4l2_fd = v4l2_fd_local;
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
        ioctl(v4l2_fd_local, VIDIOC_STREAMOFF, &type_local);
    }

    if (rgb_buffer_local) {
        free(rgb_buffer_local);
    }

    if (buffer_local != MAP_FAILED) {
        munmap(buffer_local, buf_local.length);
    }

    if (v4l2_fd_local >= 0) {
        close(v4l2_fd_local);
    }

    return ret;
}
int capture_uvc_captureImg(void) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(v4l2_fd, &fds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int ret = select(v4l2_fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        printf("等待帧超时或出错\n");
        return -1;
    }

    // 缓冲区出队
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        fprintf(stderr, "获取帧失败");
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
            if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
                fprintf(stderr, "缓冲区重新入队失败");
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
                fprintf(stderr, "JPEG解码失败");
            }
        }
    } else {
        printf("RGB缓冲区未分配\n");
        return -1;
    }
    copy_raw_data();

    // 重新将缓冲区入队以继续捕获
    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "缓冲区重新入队失败");
        return -1;
    }
    return 0;
}
void capture_uvc_clean() {
    // 检查资源有效性后再释放
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
    }

    if (rgb_buffer) {
        free(rgb_buffer);
        rgb_buffer = NULL;
    }

    if (buffer != MAP_FAILED) {
        munmap(buffer, buf.length);
        buffer = MAP_FAILED;
    }

    if (v4l2_fd >= 0) {
        close(v4l2_fd);
        v4l2_fd = -1;
    }
    // 等待1s USB正常关闭
    sleep(1);
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
#elif (USE_DMABUF)
// 保存v4l2内部所用的缓冲区指针数组
static dmabuf_buffer_t **save_used_dmabuf_buffers;
struct v4l2_buffer buf_from_dmabuf;
// dmabuf优化专用
/**
 * @brief 初始化UVC摄像头
 * @param width 捕获宽
 * @param height 捕获高
 * @param color 捕获颜色格式
 * @param alloc_buf_from_pool 从这个池申请缓冲区，池由外部创建
 * @param frames 缓冲区个数,也就是内部帧队列数量
 * @return 成功返回0 失败返回-1
 */
int capture_uvc_init_dmabuf(uint32_t width, uint32_t height,
                            enum capture_color color,
                            dmabuf_pool_t *alloc_buf_from_pool, int frames) {
    const char *camera_device = "/dev/video0";
    frames_local = frames;
    // 检查color参数
    if (color <= CAP_NONE || color >= CAP_NUMS) {
        fprintf(stderr, "颜色格式未指定或类型错误\n");
        return -1;
    }
    // 参数无误，保存到本地
    color_format = color;
    // 检查缓冲池参数
    if (!alloc_buf_from_pool) {
        fprintf(stderr, "缓冲池类型错误\n");
        return -1;
    }
    // 检查缓冲池参数
    if (alloc_buf_from_pool->capacity < frames) {
        fprintf(stderr, "缓冲池大小小于v4l2申请帧\n");
        return -1;
    }
    // 检查保存缓冲区的指针数组参数
    save_used_dmabuf_buffers = malloc(sizeof(dmabuf_buffer_t *) * frames);
    if (!save_used_dmabuf_buffers) {
        fprintf(stderr, "保存缓冲区指针数组类型错误\n");
        return -1;
    }

    // 1.打开摄像头设备
    v4l2_fd = open(camera_device, O_RDWR);
    if (v4l2_fd < 0) {
        fprintf(stderr, "打开摄像头失败\n");
        return -1;
    }
    // 2.查询设备能力
    struct v4l2_capability cap = {0};
    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "查询设备能力失败\n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }
    printf("摄像头名称: %s\n", cap.card);
    printf("驱动: %s\n", cap.driver);
    // 检查是否支持DMA-BUF导入
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "设备不支持流式I/O\n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // 3.设置视频格式
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    if (color == CAP_JPEG) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    } else if (color == CAP_YUYV) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    } else {
        fprintf(stderr, "颜色格式未指定或类型错误\n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "设置格式失败\n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    if (color == CAP_JPEG) {
        printf("实际格式: MJPEG\n");
    } else if (color == CAP_YUYV) {
        printf("实际格式: YUYV\n");
    } else {
        printf("实际格式: NULL\n");
    }

    printf("实际分辨率: %dx%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 4.计算所需缓冲区大小
    size_t buffer_size = fmt.fmt.pix.sizeimage;
    if (buffer_size == 0) {
        fprintf(stderr, "缓冲区大小获取失败\n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // 5.申请dmabuf
    for (int i = 0; i < frames; i++) {
        // 申请缓冲区
        dmabuf_buffer_t *buffer =
            dmabuf_buffer_alloc(alloc_buf_from_pool, buffer_size);

        if (!buffer) {
            fprintf(stderr, "从缓冲池申请缓冲区失败");
            // 释放所有已分配的缓冲区
            for (int j = 0; j < i; j++) {
                dmabuf_buffer_free(alloc_buf_from_pool,
                                   save_used_dmabuf_buffers[j]);
                // 清空指针数组
                save_used_dmabuf_buffers[j] = NULL;
            }
            free(save_used_dmabuf_buffers);
            save_used_dmabuf_buffers = NULL;
            close(v4l2_fd);
            v4l2_fd = -1;
            return -1;
        }
        // 申请成功，保存缓冲区指针
        save_used_dmabuf_buffers[i] = buffer;
    }

    // 6.请求dmabuf缓冲区
    struct v4l2_requestbuffers reqbuf = {0};
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_DMABUF;
    reqbuf.count = frames;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        fprintf(stderr, "请求DMA-BUF缓冲区失败");
        // 释放所有已分配的缓冲区
        for (int i = 0; i < frames; i++) {
            dmabuf_buffer_free(alloc_buf_from_pool,
                               save_used_dmabuf_buffers[i]);
            // 清空指针数组
            save_used_dmabuf_buffers[i] = NULL;
        }
        free(save_used_dmabuf_buffers);
        save_used_dmabuf_buffers = NULL;
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // 7.创建v4l2缓冲区并导入dmabuf
    memset(&buf_from_dmabuf, 0, sizeof(buf_from_dmabuf));
    for (int i = 0; i < frames; i++) {
        buf_from_dmabuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf_from_dmabuf.memory = V4L2_MEMORY_DMABUF;
        buf_from_dmabuf.index = i;
        buf_from_dmabuf.m.fd = save_used_dmabuf_buffers[i]->dmabuf_fd;

        // 将缓冲区加入队列
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf_from_dmabuf) < 0) {
            fprintf(stderr, "DMA-BUF缓冲区入队失败");
            // 先释放已成功入队的缓冲区
            for (int j = 0; j < i; j++) {
                // 已入队的缓冲区（0 到 i-1）：需要先解除驱动引用，后面一起释放
                if (save_used_dmabuf_buffers[j]) {
                    dmabuf_unref(save_used_dmabuf_buffers[j]);
                }
            }
            // 前面解除了之前入队成功的缓冲区的引用，这里直接释放全部
            for (int j = 0; j < frames; j++) {
                if (save_used_dmabuf_buffers[j]) {
                    dmabuf_buffer_free(alloc_buf_from_pool,
                                       save_used_dmabuf_buffers[j]);
                    save_used_dmabuf_buffers[j] = NULL;
                }
            }
            free(save_used_dmabuf_buffers);
            save_used_dmabuf_buffers = NULL;
            close(v4l2_fd);
            v4l2_fd = -1;
            return -1;
        }
        // 增加引用计数，缓冲区除缓冲池外还被v4l2驱动持有
        dmabuf_ref(save_used_dmabuf_buffers[i]);
    }

    // 8.开始捕获
    memset(&type, 0, sizeof(type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "开始流失败");
        if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
            // 即使关闭失败也要继续清理，但记录日志
            fprintf(stderr, "关闭流失败，继续清理");
        }
        // 释放所有缓冲区
        for (int i = 0; i < frames; i++) {
            // 释放前取消引用计数，将缓冲区还给缓冲池
            if (save_used_dmabuf_buffers[i]) {
                dmabuf_unref(save_used_dmabuf_buffers[i]);
                dmabuf_buffer_free(alloc_buf_from_pool,
                                   save_used_dmabuf_buffers[i]);
                // 清空指针数组
                save_used_dmabuf_buffers[i] = NULL;
            }
        }
        free(save_used_dmabuf_buffers);
        save_used_dmabuf_buffers = NULL;
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    printf("摄像头DMA-BUF初始化成功\n");
    return 0;
}
/**
 * @brief 捕获一帧
 * @param next_buffer 下一个入队的缓冲区
 * @return dmabuf_buffer_t* 本次捕获获取的数据缓冲区，失败NULL
 */
dmabuf_buffer_t *capture_uvc_captureImg_dmabuf(dmabuf_buffer_t *next_buffer) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(v4l2_fd, &fds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    int ret = select(v4l2_fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        printf("等待帧超时或出错\n");
        return NULL;
    }

    // 检查参数
    if (v4l2_fd < 0 || frames_local == 0) {
        fprintf(stderr, "摄像头未初始化\n");
        return NULL;
    }
    // 1.取出已填充数据的缓冲区
    struct v4l2_buffer filled_buf = {0};
    filled_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    filled_buf.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &filled_buf) < 0) {
        fprintf(stderr, "取出缓冲区失败");
        return NULL;
    }
    if (filled_buf.index >= frames_local) {
        fprintf(stderr, "无效缓冲区索引: %u\n", filled_buf.index);
        // 尝试重新入队（避免队列少缓冲区）
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &filled_buf) < 0)
            fprintf(stderr, "重新入队失败");
        return NULL;
    }
    // 2. 获取已填充的缓冲区
    dmabuf_buffer_t *filled_buffer = save_used_dmabuf_buffers[filled_buf.index];
    if (!filled_buffer) {
        fprintf(stderr, "缓冲区指针为空: index=%u\n", filled_buf.index);
        return NULL;
    }
    dmabuf_unref(filled_buffer); // 从v4l2驱动取出成功，取消引用

    // 3. 决定下一个入队的缓冲区，通过检查next_buffer是否存在来决定
    if (!next_buffer) {
        fprintf(stderr, "没有传入下一个入队的缓冲区\n");
        return NULL;
    }

    // 4. 将缓冲区入队
    struct v4l2_buffer qbuf = {0};
    qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qbuf.memory = V4L2_MEMORY_DMABUF;
    qbuf.index = filled_buf.index;
    qbuf.m.fd = next_buffer->dmabuf_fd;
    // 新缓冲区入队前增加引用计数（驱动将持有）
    dmabuf_ref(next_buffer);

    if (ioctl(v4l2_fd, VIDIOC_QBUF, &qbuf) < 0) {
        fprintf(stderr, "缓冲区入队失败");
        // 如果使用了新缓冲区，恢复原状
        // 减少新缓冲区的引用计数（因为驱动不会持有了）
        dmabuf_unref(next_buffer);
        // 尝试将原 filled_buffer 重新入队以恢复队列
        qbuf.m.fd = filled_buffer->dmabuf_fd;
        dmabuf_ref(filled_buffer); // 驱动将重新持有
        // 尝试将原缓冲区入队
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &qbuf) < 0) {
            fprintf(stderr, "严重错误：无法恢复队列");
            dmabuf_unref(filled_buffer);
        }
        return NULL;
    }
    // 5. 更新参数
    if (next_buffer) {
        // 更新数组指针为新缓冲区
        save_used_dmabuf_buffers[filled_buf.index] = next_buffer;
    }

    // 6. 返回已填充的缓冲区
    // dmabuf_ref(filled_buffer); // 返回前，为调用者增加一次引用计数
    return filled_buffer;
}
/**
 * @brief 清理v4l2资源
 * @param alloc_buf_from_pool 初始化时使用的缓冲池，内部将缓冲区释放回缓冲池
 */
void capture_uvc_clean_dmabuf(dmabuf_pool_t *alloc_buf_from_pool) {
    if (v4l2_fd < 0 || frames_local == 0) {
        fprintf(stderr, "摄像头未初始化\n");
        return;
    }
    // 检查缓冲池参数
    if (!alloc_buf_from_pool) {
        fprintf(stderr, "缓冲池类型错误\n");
        return;
    }
    // 检查保存缓冲区的指针数组参数
    if (!save_used_dmabuf_buffers) {
        fprintf(stderr, "保存缓冲区指针数组类型错误\n");
        return;
    }
    // 1.停止视频流
    if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
        fprintf(stderr, "停止视频流失败\n");
    }
    // 2.清理v4l2使用的缓冲区
    dmabuf_buffer_t *buffer = NULL;
    for (int i = 0; i < frames_local; i++) {
        buffer = save_used_dmabuf_buffers[i];
        if (!buffer)
            continue;
        // 无论当前 ref_count 是多少，都减去驱动曾经持有的那一次
        dmabuf_unref(buffer); // 2 → 1
        // 此时若 ref_count == 1（仅池持有），dmabuf_buffer_free 会释放内存
        // 若 ref_count > 1（调用者仍持有），则仅递减，内存暂不释放
        dmabuf_buffer_free(alloc_buf_from_pool, buffer);
        save_used_dmabuf_buffers[i] = NULL;
    }
    // 3. 关闭设备
    close(v4l2_fd);
    v4l2_fd = -1;
    // 4. 重置其他全局状态
    color_format = CAP_NONE;
    frames_local = 0;
    memset(&fmt, 0, sizeof(fmt));
    memset(&type, 0, sizeof(type));
    memset(&buf_from_dmabuf, 0, sizeof(buf_from_dmabuf));
    free(save_used_dmabuf_buffers);
    save_used_dmabuf_buffers = NULL;
    // 等待1s USB正常关闭
    sleep(1);
    printf("捕获结束!\n");
}
size_t capture_uvc_get_v4l2buf_size(void) { return fmt.fmt.pix.sizeimage; }
#endif