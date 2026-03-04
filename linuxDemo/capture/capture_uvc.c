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
// 保存v4l2内部所用的缓冲区指针数组
static dmabuf_buffer_t **save_used_buffers = NULL;
static struct v4l2_buffer v4l2_buf;
/**
 * @brief 初始化UVC摄像头
 * @param width 捕获宽
 * @param height 捕获高
 * @param color 捕获颜色格式
 * @param alloc_buf_from_pool 从这个池申请缓冲区，池由外部创建
 * @param frames 缓冲区个数,也就是内部帧队列数量
 * @param framerate 帧率
 * @return 成功返回0 失败返回-1
 */
int capture_uvc_init(uint32_t width, uint32_t height, enum capture_color color,
                     dmabuf_pool_t *alloc_buf_from_pool, int frames,
                     int framerate) {
    const char *camera_device = "/dev/video0";
    frames_local = frames;
    int buf_alloc_cnt = 0; // 已分配的缓冲区个数
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
    save_used_buffers = malloc(sizeof(dmabuf_buffer_t *) * frames);
    if (!save_used_buffers) {
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
        goto error_close;
    }
    printf("摄像头名称: %s\n", cap.card);
    printf("驱动: %s\n", cap.driver);
#if (USE_MALLOC)
#elif (USE_DMABUF)
    // 检查是否支持DMA-BUF导入
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "设备不支持流式I/O\n");
        goto error_close;
    }
#endif

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
        goto error_close;
    }
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "设置格式失败\n");
        goto error_close;
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
        goto error_close;
    }

    // 5.申请缓冲区
    for (int i = 0; i < frames; i++) {
        // 申请缓冲区，每个buffer都已被驱动持有
        dmabuf_buffer_t *buffer =
            dmabuf_buffer_alloc(alloc_buf_from_pool, buffer_size);

        if (!buffer) {
            fprintf(stderr, "从缓冲池申请缓冲区失败");
            // 无视引用计数，强制释放所有已分配缓冲区
            goto error_free_buffers;
        }
        // 申请成功，保存缓冲区指针
        save_used_buffers[i] = buffer;
        buf_alloc_cnt++;
    }

    // 6.请求dmabuf缓冲区
    struct v4l2_requestbuffers reqbuf = {0};
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
#if (USE_MALLOC)
    reqbuf.memory = V4L2_MEMORY_USERPTR;
#elif (USE_DMABUF)
    reqbuf.memory = V4L2_MEMORY_DMABUF;
#endif

    reqbuf.count = frames;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        fprintf(stderr, "请求缓冲区失败");
        // 无视引用计数，强制释放所有已分配缓冲区
        goto error_free_buffers;
    }

    // 7.创建v4l2缓冲区并导入dmabuf
    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    for (int i = 0; i < frames; i++) {
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4l2_buf.index = i;
#if (USE_MALLOC)
        v4l2_buf.memory = V4L2_MEMORY_USERPTR;
        v4l2_buf.m.userptr = (unsigned long)save_used_buffers[i]->data;
#elif (USE_DMABUF)
        v4l2_buf.memory = V4L2_MEMORY_DMABUF;
        v4l2_buf.m.fd = save_used_buffers[i]->dmabuf_fd;
#endif
        v4l2_buf.length = save_used_buffers[i]->size;

        // 将缓冲区加入队列
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &v4l2_buf) < 0) {
            fprintf(stderr, "缓冲区入队失败");
            // 无视引用计数，强制释放所有已分配缓冲区
            goto error_free_buffers;
        }
    }

    // 7.5 设置帧率
    if (framerate > 0) {
        struct v4l2_streamparm parm = {0};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = framerate;
        if (ioctl(v4l2_fd, VIDIOC_S_PARM, &parm) < 0) {
            fprintf(stderr, "设置帧率失败，继续初始化\n");
        }
    }
    // 8.开始捕获
    memset(&type, 0, sizeof(type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "开始流失败");
        // 尝试停止流
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
        // 即使关闭失败也要继续清理，但记录日志
        fprintf(stderr, "关闭流失败，继续清理");
        // 无视引用计数，强制释放所有已分配缓冲区
        goto error_free_buffers;
    }
#if (USE_MALLOC)
    printf("摄像头MALLOC-BUF初始化成功\n");
#elif (USE_DMABUF)
    printf("摄像头DMA-BUF初始化成功\n");
#endif

    return 0;

error_free_buffers:
    if (save_used_buffers) {
        for (int i = 0; i < buf_alloc_cnt; i++) {
            dmabuf_buffer_force_free(alloc_buf_from_pool, save_used_buffers[i]);
        }
        free(save_used_buffers);
        save_used_buffers = NULL;
    }
    // 继续处理
error_close:
    if (v4l2_fd >= 0) {
        close(v4l2_fd);
        v4l2_fd = -1;
    }
    return -1;
}
/**
 * @brief
 * 捕获一帧，成功时管理返回的缓冲区引用计数，对调用者默认加一次引用，失败管理传入的缓冲区引用计数
 * @param next_buffer 下一个入队的缓冲区
 * @return dmabuf_buffer_t* 本次捕获获取的数据缓冲区，失败NULL
 */
dmabuf_buffer_t *capture_uvc_captureImg(dmabuf_buffer_t *next_buffer) {
    // 检查设备状态
    if (v4l2_fd < 0 || frames_local == 0) {
        fprintf(stderr, "摄像头未初始化\n");
        return NULL;
    }
    // 检查下一个入队的缓冲区，通过检查next_buffer是否存在来决定
    /*******简要说明next_buffer*******/
    // 传入的next_buffer引用计数=2 池，驱动持有
    // 如果驱动入新缓冲区成功，next_buffer被入队驱动，引用计数不变
    // 如果驱动入新缓冲区失败，返回NULL，next_buffer由外部取消引用
    if (!next_buffer) {
        // fprintf(stderr, "没有传入下一个入队的缓冲区\n");
        return NULL;
    }
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

    // 1.取出已填充数据的缓冲区
    struct v4l2_buffer filled_v4l2_buf = {0};
    filled_v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    filled_v4l2_buf.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &filled_v4l2_buf) < 0) {
        fprintf(stderr, "取出缓冲区失败");
        return NULL;
    }
    if (filled_v4l2_buf.index >= frames_local) {
        fprintf(stderr, "无效缓冲区索引: %u\n", filled_v4l2_buf.index);
        // 尝试重新入队（避免队列少缓冲区）
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &filled_v4l2_buf) < 0)
            fprintf(stderr, "重新入队失败");
        return NULL;
    }
    // 2. 获取已填充的缓冲区
    dmabuf_buffer_t *filled_buffer =
        save_used_buffers[filled_v4l2_buf.index]; // 利用索引获取
    /*******简要说明filled_buffer*******/
    // filled_buffer引用计数=2 池，驱动持有
    // 如果驱动入新缓冲区成功，filled_buffer被返回给外部，外部取消驱动对其的引用
    // 如果驱动入新缓冲区失败，filled_buffer被重入队驱动，引用计数不变

    if (!filled_buffer) {
        fprintf(stderr, "缓冲区指针为空: index=%u\n", filled_v4l2_buf.index);
        return NULL;
    }
    // ========== 引用转换开始 ==========
    // 转换 filled_buffer 的所有权：驱动 → 调用者
    // 当前 filled_buffer 的 ref = 2（池+驱动）
    dmabuf_ref(filled_buffer);   // 增加调用者引用：2 → 3（池+驱动+调用者占位）
    dmabuf_unref(filled_buffer); // 释放驱动引用：3 → 2（池+调用者占位）
    // 此时 filled_buffer 的 ref 仍为 2，但持有者变为：池 + 调用者占位
    // 调用者占位引用即将在返回时成为调用者的有效引用

    // 3. 将缓冲区入队
    struct v4l2_buffer qbuf = {0};
    qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qbuf.index = filled_v4l2_buf.index;
#if (USE_MALLOC)
    qbuf.memory = V4L2_MEMORY_USERPTR;
    qbuf.m.userptr = (unsigned long)next_buffer->data;
#elif (USE_DMABUF)
    qbuf.memory = V4L2_MEMORY_DMABUF;
    qbuf.m.fd = next_buffer->dmabuf_fd;
#endif
    qbuf.length = next_buffer->size;

    if (ioctl(v4l2_fd, VIDIOC_QBUF, &qbuf) < 0) {
        fprintf(stderr, "缓冲区入队失败");
        // 尝试入队新缓冲区失败，入队之前取出的缓冲区
        // 尝试将原 filled_buffer 重新入队以恢复队列
#if (USE_MALLOC)
        qbuf.m.userptr = (unsigned long)filled_buffer->data;
#elif (USE_DMABUF)
        qbuf.m.fd = filled_buffer->dmabuf_fd;
#endif
        // 尝试将原缓冲区入队
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &qbuf) < 0) {
            fprintf(stderr, "严重错误：无法恢复队列");
        }
        // 恢复 filled_buffer 的驱动持有
        dmabuf_ref(filled_buffer); // 增加驱动引用：2 → 3（池+调用者占位+驱动）
        dmabuf_unref(filled_buffer); // 释放调用者占位：3 → 2（池+驱动）
        return NULL;
    }
    // 入队成功：转换 next_buffer 的所有权：调用者 → 驱动
    dmabuf_ref(next_buffer);   // 增加驱动引用：2 → 3（池+调用者+驱动）
    dmabuf_unref(next_buffer); // 释放调用者引用：3 → 2（池+驱动）
    // 此时 next_buffer 的 ref 仍为 2，但持有者变为：池 + 驱动

    // 4. 更新数组指针为新缓冲区
    save_used_buffers[filled_v4l2_buf.index] = next_buffer;

    // 5. 返回已填充的缓冲区，其 ref=2（池+调用者），调用者获得一次有效引用
    return filled_buffer;
}
/**
 * @brief 清理v4l2资源
 * @param alloc_buf_from_pool 初始化时使用的缓冲池，内部将缓冲区释放回缓冲池
 */
void capture_uvc_clean(dmabuf_pool_t *alloc_buf_from_pool) {
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
    if (!save_used_buffers) {
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
        buffer = save_used_buffers[i];
        if (!buffer)
            continue;
        // 无论当前 ref_count 是多少，都减去驱动曾经持有的那一次
        dmabuf_unref(buffer); // 2 → 1
        // 此时若 ref_count == 1（仅池持有），dmabuf_buffer_free 会释放内存
        // 若 ref_count > 1（调用者仍持有），则仅递减，内存暂不释放
        dmabuf_buffer_free(alloc_buf_from_pool, buffer);
        save_used_buffers[i] = NULL;
    }
    // 3. 关闭设备
    close(v4l2_fd);
    v4l2_fd = -1;
    // 4. 重置其他全局状态
    color_format = CAP_NONE;
    frames_local = 0;
    memset(&fmt, 0, sizeof(fmt));
    memset(&type, 0, sizeof(type));
    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    free(save_used_buffers);
    save_used_buffers = NULL;
    // 等待1s USB正常关闭
    sleep(1);
    printf("捕获结束!\n");
}
void capture_uvc_set_camera(bool enable_auto_exposure, int fixed_exposure_time,
                            bool enable_dynamic_framerate) {
    if (v4l2_fd < 0 || frames_local == 0) {
        fprintf(stderr, "摄像头未初始化\n");
        return;
    }
    struct v4l2_control ctrl;

    // 设置自动曝光
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    if (enable_auto_exposure) {
        ctrl.value = V4L2_EXPOSURE_AUTO;
    } else {
        ctrl.value = V4L2_EXPOSURE_MANUAL;
    }
    if (ioctl(v4l2_fd, VIDIOC_S_CTRL, &ctrl) < 0)
        fprintf(stderr, "设置曝光失败\n");

    // 设置动态帧率
    ctrl.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
    ctrl.value = (int)enable_dynamic_framerate;
    ioctl(v4l2_fd, VIDIOC_S_CTRL, &ctrl); // 忽略错误（部分驱动可能不支持）

    // 设置曝光时间
    if (!enable_auto_exposure) {
        ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        ctrl.value = fixed_exposure_time;
        if (ioctl(v4l2_fd, VIDIOC_S_CTRL, &ctrl) < 0)
            fprintf(stderr, "设置曝光时间失败\n");
    }
    printf("摄像头参数设置成功\n");
}
size_t capture_uvc_get_v4l2buf_size(void) { return fmt.fmt.pix.sizeimage; }