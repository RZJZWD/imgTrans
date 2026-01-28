/*
 * v4l2_capability_tester.c
 * 完整的V4L2支持验证程序
 * 编译: gcc -o v4l2_test v4l2_capability_tester.c
 * 运行: ./v4l2_test /dev/video0
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/video0"
#define TEST_WIDTH 640
#define TEST_HEIGHT 480
#define TEST_FORMAT V4L2_PIX_FMT_YUYV
#define BUFFER_COUNT 4

typedef struct {
    void *start;
    size_t length;
} Buffer;

// 颜色输出
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_RESET "\033[0m"

// 测试结果
typedef enum {
    TEST_UNKNOWN,
    TEST_PASSED,
    TEST_FAILED,
    TEST_NOT_SUPPORTED
} TestResult;

// 打印测试结果
void print_test_result(const char *test_name, TestResult result) {
    const char *color = COLOR_RESET;
    const char *status = "UNKNOWN";

    switch (result) {
    case TEST_PASSED:
        color = COLOR_GREEN;
        status = "PASSED";
        break;
    case TEST_FAILED:
        color = COLOR_RED;
        status = "FAILED";
        break;
    case TEST_NOT_SUPPORTED:
        color = COLOR_YELLOW;
        status = "NOT SUPPORTED";
        break;
    default:
        color = COLOR_RESET;
        status = "UNKNOWN";
    }

    printf("%-30s [%s%s%s]\n", test_name, color, status, COLOR_RESET);
}

// 打印能力标志
void print_capabilities(const char *title, unsigned int caps) {
    printf("\n%s (0x%08x):\n", title, caps);

    struct cap_info {
        unsigned int flag;
        const char *name;
        const char *description;
    } capabilities[] = {
        {V4L2_CAP_VIDEO_CAPTURE, "Video Capture", "捕获视频"},
        {V4L2_CAP_VIDEO_OUTPUT, "Video Output", "输出视频"},
        {V4L2_CAP_VIDEO_OVERLAY, "Video Overlay", "视频叠加"},
        {V4L2_CAP_VBI_CAPTURE, "VBI Capture", "VBI捕获"},
        {V4L2_CAP_VBI_OUTPUT, "VBI Output", "VBI输出"},
        {V4L2_CAP_SLICED_VBI_CAPTURE, "Sliced VBI Capture", "切片VBI捕获"},
        {V4L2_CAP_SLICED_VBI_OUTPUT, "Sliced VBI Output", "切片VBI输出"},
        {V4L2_CAP_RDS_CAPTURE, "RDS Capture", "RDS捕获"},
        {V4L2_CAP_VIDEO_OUTPUT_OVERLAY, "Video Output Overlay", "视频输出叠加"},
        {V4L2_CAP_HW_FREQ_SEEK, "HW Frequency Seek", "硬件频率搜索"},
        {V4L2_CAP_RADIO, "Radio", "收音机"},
        {V4L2_CAP_TUNER, "Tuner", "调谐器"},
        {V4L2_CAP_AUDIO, "Audio", "音频"},
        {V4L2_CAP_READWRITE, "Read/Write", "读写IO"},
        {V4L2_CAP_ASYNCIO, "Async IO", "异步IO"},
        {V4L2_CAP_STREAMING, "Streaming", "流式IO"},
        {V4L2_CAP_DEVICE_CAPS, "Device Caps", "设备能力标志"},
        {V4L2_CAP_VIDEO_CAPTURE_MPLANE, "Video Capture Mplane",
         "多平面视频捕获"},
        {V4L2_CAP_VIDEO_OUTPUT_MPLANE, "Video Output Mplane", "多平面视频输出"},
        {V4L2_CAP_VIDEO_M2M_MPLANE, "Video M2M Mplane", "多平面内存到内存"},
        {V4L2_CAP_VIDEO_M2M, "Video M2M", "内存到内存"},
        {V4L2_CAP_EXT_PIX_FORMAT, "Ext Pix Format", "扩展像素格式"},
        {0, NULL, NULL}};

    for (int i = 0; capabilities[i].name != NULL; i++) {
        if (caps & capabilities[i].flag) {
            printf("  ✓ %-25s - %s\n", capabilities[i].name,
                   capabilities[i].description);
        }
    }
}

// 测试1: 基本能力查询
TestResult test_basic_capabilities(int fd) {
    struct v4l2_capability cap;

    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        return TEST_FAILED;
    }

    printf("\n" COLOR_BLUE "=== 设备基本信息 ===" COLOR_RESET "\n");
    printf("设备驱动: %s\n", cap.driver);
    printf("设备名称: %s\n", cap.card);
    printf("总线信息: %s\n", cap.bus_info);
    printf("驱动版本: %u.%u.%u\n", (cap.version >> 16) & 0xFF,
           (cap.version >> 8) & 0xFF, cap.version & 0xFF);

    print_capabilities("基本能力", cap.capabilities);

    if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
        print_capabilities("设备特定能力", cap.device_caps);
    }

    // 检查是否支持视频捕获
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        printf(COLOR_RED "错误: 设备不支持视频捕获!\n" COLOR_RESET);
        return TEST_FAILED;
    }

    // 检查是否支持流式IO
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        printf(COLOR_YELLOW "警告: 设备不支持流式IO\n" COLOR_RESET);
    }

    return TEST_PASSED;
}

// 测试2: 像素格式支持
TestResult test_pixel_formats(int fd) {
    struct v4l2_fmtdesc fmt;
    int format_count = 0;

    printf("\n" COLOR_BLUE "=== 支持的像素格式 ===" COLOR_RESET "\n");

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (fmt.index = 0;; fmt.index++) {
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
            if (fmt.index == 0) {
                printf("未找到支持的格式\n");
                return TEST_FAILED;
            }
            break;
        }

        format_count++;
        printf("  %2d. %-20s (%.4s)\n", fmt.index, fmt.description,
               (char *)&fmt.pixelformat);

        // 枚举支持的帧大小
        struct v4l2_frmsizeenum frmsize;
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = fmt.pixelformat;

        for (frmsize.index = 0;; frmsize.index++) {
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0) {
                break;
            }

            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                printf("      %dx%d\n", frmsize.discrete.width,
                       frmsize.discrete.height);
            } else if (frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                printf("      连续范围: %dx%d 到 %dx%d\n",
                       frmsize.stepwise.min_width, frmsize.stepwise.min_height,
                       frmsize.stepwise.max_width, frmsize.stepwise.max_height);
            } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                printf("      步进范围: %dx%d 到 %dx%d (步进 %dx%d)\n",
                       frmsize.stepwise.min_width, frmsize.stepwise.min_height,
                       frmsize.stepwise.max_width, frmsize.stepwise.max_height,
                       frmsize.stepwise.step_width,
                       frmsize.stepwise.step_height);
            }
        }
    }

    if (format_count == 0) {
        return TEST_FAILED;
    }

    return TEST_PASSED;
}

// 测试3: MMAP模式支持
TestResult test_mmap_support(int fd) {
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_buffer buf;
    Buffer *buffers = NULL;
    int i;

    printf("\n" COLOR_BLUE "=== 测试MMAP模式 ===" COLOR_RESET "\n");

    // 首先设置格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = TEST_WIDTH;
    fmt.fmt.pix.height = TEST_HEIGHT;
    fmt.fmt.pix.pixelformat = TEST_FORMAT;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("设置格式失败: %s\n", strerror(errno));
        return TEST_NOT_SUPPORTED;
    }

    printf("设置格式: %dx%d, 格式: %.4s\n", fmt.fmt.pix.width,
           fmt.fmt.pix.height, (char *)&fmt.fmt.pix.pixelformat);

    // 请求MMAP缓冲区
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = BUFFER_COUNT;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        printf("请求MMAP缓冲区失败: %s\n", strerror(errno));
        return TEST_NOT_SUPPORTED;
    }

    printf("分配了 %d 个MMAP缓冲区\n", reqbuf.count);

    if (reqbuf.count < 1) {
        printf("错误: 缓冲区数量为0\n");
        return TEST_FAILED;
    }

    // 分配缓冲区结构
    buffers = calloc(reqbuf.count, sizeof(Buffer));
    if (!buffers) {
        perror("分配缓冲区内存失败");
        return TEST_FAILED;
    }

    // 查询并映射每个缓冲区
    for (i = 0; i < reqbuf.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            printf("查询缓冲区 %d 失败: %s\n", i, strerror(errno));
            free(buffers);
            return TEST_FAILED;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED) {
            printf("映射缓冲区 %d 失败: %s\n", i, strerror(errno));
            // 清理之前成功映射的缓冲区
            for (int j = 0; j < i; j++) {
                munmap(buffers[j].start, buffers[j].length);
            }
            free(buffers);
            return TEST_FAILED;
        }

        printf("  缓冲区 %d: 地址=%p, 大小=%zu\n", i, buffers[i].start,
               buffers[i].length);
    }

    // 测试入队/出队操作
    printf("\n测试缓冲区入队/出队...\n");

    // 将所有缓冲区入队
    for (i = 0; i < reqbuf.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            printf("入队缓冲区 %d 失败: %s\n", i, strerror(errno));
            goto cleanup;
        }
    }

    // 启动流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("启动流失败: %s\n", strerror(errno));
        goto cleanup;
    }

    // 尝试读取一帧（带超时）
    fd_set fds;
    struct timeval tv;
    int ret;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    ret = select(fd + 1, &fds, NULL, NULL, &tv);

    if (ret == -1) {
        printf("select错误: %s\n", strerror(errno));
        goto stop_stream;
    } else if (ret == 0) {
        printf("超时: 没有可读的数据\n");
        goto stop_stream;
    }

    // 读取缓冲区
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        printf("出队缓冲区失败: %s\n", strerror(errno));
        goto stop_stream;
    }

    printf("成功捕获一帧数据!\n");
    printf("  缓冲区索引: %d\n", buf.index);
    printf("  数据大小: %u 字节\n", buf.bytesused);
    printf("  序列号: %u\n", buf.sequence);

    // 将缓冲区重新入队
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        printf("重新入队缓冲区失败: %s\n", strerror(errno));
        goto stop_stream;
    }

    // 停止流
stop_stream:
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    // 清理
cleanup:
    // 取消映射
    for (i = 0; i < reqbuf.count; i++) {
        if (buffers[i].start != MAP_FAILED) {
            munmap(buffers[i].start, buffers[i].length);
        }
    }

    free(buffers);

    // 释放所有缓冲区
    reqbuf.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &reqbuf);

    return TEST_PASSED;
}

// 测试4: USERPTR模式支持
TestResult test_userptr_support(int fd) {
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_buffer buf;
    void *user_buffer = NULL;
    size_t buffer_size;

    printf("\n" COLOR_BLUE "=== 测试USERPTR模式 ===" COLOR_RESET "\n");

    // 获取需要的缓冲区大小
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("获取格式失败: %s\n", strerror(errno));
        // 尝试设置一个默认格式
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = TEST_WIDTH;
        fmt.fmt.pix.height = TEST_HEIGHT;
        fmt.fmt.pix.pixelformat = TEST_FORMAT;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            printf("设置格式失败: %s\n", strerror(errno));
            return TEST_NOT_SUPPORTED;
        }
    }

    buffer_size = fmt.fmt.pix.sizeimage;
    if (buffer_size == 0) {
        buffer_size = fmt.fmt.pix.width * fmt.fmt.pix.height * 2; // YUYV估算
    }

    printf("预计缓冲区大小: %zu 字节\n", buffer_size);

    // 请求USERPTR缓冲区
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = 1;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        printf("请求USERPTR缓冲区失败: %s\n", strerror(errno));
        return TEST_NOT_SUPPORTED;
    }

    printf("USERPTR缓冲区请求成功\n");

    // 分配用户空间缓冲区（页对齐）
    if (posix_memalign(&user_buffer, getpagesize(), buffer_size) != 0) {
        printf("分配对齐内存失败\n");
        return TEST_FAILED;
    }

    printf("分配用户空间缓冲区: %p (大小: %zu)\n", user_buffer, buffer_size);

    // 准备缓冲区
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.index = 0;
    buf.m.userptr = (unsigned long)user_buffer;
    buf.length = buffer_size;

    // 入队缓冲区
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        printf("入队USERPTR缓冲区失败: %s\n", strerror(errno));
        free(user_buffer);
        return TEST_FAILED;
    }

    printf("USERPTR缓冲区入队成功\n");

    // 清理
    free(user_buffer);

    // 释放缓冲区
    reqbuf.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &reqbuf);

    return TEST_PASSED;
}

// 测试5: DMABUF模式支持
TestResult test_dmabuf_support(int fd) {
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_exportbuffer expbuf;
    int dmabuf_fd = -1;

    printf("\n" COLOR_BLUE "=== 测试DMABUF模式 ===" COLOR_RESET "\n");

    // 请求DMABUF缓冲区
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = 1;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_DMABUF;

    if (ioctl(fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        printf("请求DMABUF缓冲区失败: %s\n", strerror(errno));
        return TEST_NOT_SUPPORTED;
    }

    printf("DMABUF缓冲区请求成功\n");

    // 尝试导出缓冲区（测试DMABUF导出功能）
    memset(&expbuf, 0, sizeof(expbuf));
    expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    expbuf.index = 0;
    expbuf.plane = 0;
    expbuf.flags = O_RDWR;

    if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        printf("导出DMABUF失败: %s\n", strerror(errno));
        // 这不代表DMABUF完全不可用，可能只是导出功能受限
        printf("注意: 可以导入DMABUF，但不能导出\n");
    } else {
        dmabuf_fd = expbuf.fd;
        printf("DMABUF导出成功，文件描述符: %d\n", dmabuf_fd);

        // 测试导入（重新导入到V4L2）
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.index = 0;
        buf.m.fd = dmabuf_fd;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            printf("导入DMABUF失败: %s\n", strerror(errno));
        } else {
            printf("DMABUF导入成功\n");
        }

        close(dmabuf_fd);
    }

    // 清理
    reqbuf.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &reqbuf);

    return TEST_PASSED;
}

// 测试6: CREATE_BUFS支持（高级功能）
TestResult test_createbufs_support(int fd) {
    struct v4l2_create_buffers createbuf;

    printf("\n" COLOR_BLUE "=== 测试CREATE_BUFS功能 ===" COLOR_RESET "\n");

    memset(&createbuf, 0, sizeof(createbuf));
    createbuf.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    createbuf.format.fmt.pix.width = TEST_WIDTH;
    createbuf.format.fmt.pix.height = TEST_HEIGHT;
    createbuf.format.fmt.pix.pixelformat = TEST_FORMAT;
    createbuf.format.fmt.pix.field = V4L2_FIELD_NONE;
    createbuf.memory = V4L2_MEMORY_MMAP;
    createbuf.count = 2;

    if (ioctl(fd, VIDIOC_CREATE_BUFS, &createbuf) < 0) {
        printf("CREATE_BUFS失败: %s\n", strerror(errno));
        return TEST_NOT_SUPPORTED;
    }

    printf("CREATE_BUFS成功\n");
    printf("  实际分配的缓冲区数: %d\n", createbuf.count);
    printf("  格式: %dx%d, %.4s\n", createbuf.format.fmt.pix.width,
           createbuf.format.fmt.pix.height,
           (char *)&createbuf.format.fmt.pix.pixelformat);

    // 清理
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = 0;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &reqbuf);

    return TEST_PASSED;
}

// 主函数
int main(int argc, char *argv[]) {
    const char *device_path = DEFAULT_DEVICE;
    int fd;

    if (argc > 1) {
        device_path = argv[1];
    }

    printf(COLOR_BLUE "\n========================================" COLOR_RESET);
    printf(COLOR_BLUE "\n    V4L2 支持验证工具\n" COLOR_RESET);
    printf(COLOR_BLUE "========================================\n" COLOR_RESET);
    printf("测试设备: %s\n\n", device_path);

    // 打开设备
    fd = open(device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, COLOR_RED "无法打开设备 %s: %s\n" COLOR_RESET,
                device_path, strerror(errno));

        // 尝试以只读方式打开
        fd = open(device_path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, COLOR_RED "也无法以只读方式打开设备\n" COLOR_RESET);
            return EXIT_FAILURE;
        }
        printf(COLOR_YELLOW
               "警告: 以只读方式打开设备，某些功能可能受限\n" COLOR_RESET);
    }

    // 执行所有测试
    TestResult results[6];

    results[0] = test_basic_capabilities(fd);
    results[1] = test_pixel_formats(fd);
    results[2] = test_mmap_support(fd);
    results[3] = test_userptr_support(fd);
    results[4] = test_dmabuf_support(fd);
    results[5] = test_createbufs_support(fd);

    // 打印测试总结
    printf(COLOR_BLUE
           "\n========================================\n" COLOR_RESET);
    printf(COLOR_BLUE "            测试结果总结\n" COLOR_RESET);
    printf(COLOR_BLUE "========================================\n" COLOR_RESET);

    print_test_result("基本能力查询", results[0]);
    print_test_result("像素格式支持", results[1]);
    print_test_result("MMAP模式支持", results[2]);
    print_test_result("USERPTR模式支持", results[3]);
    print_test_result("DMABUF模式支持", results[4]);
    print_test_result("CREATE_BUFS支持", results[5]);

    // 提供建议
    printf(COLOR_BLUE
           "\n========================================\n" COLOR_RESET);
    printf(COLOR_BLUE "            开发建议\n" COLOR_RESET);
    printf(COLOR_BLUE "========================================\n" COLOR_RESET);

    if (results[2] == TEST_PASSED) {
        printf(COLOR_GREEN "✓ 推荐使用MMAP模式:\n" COLOR_RESET);
        printf("  优势: 零拷贝到用户空间，性能良好\n");
        printf("  适用: 大部分应用场景\n");
    }

    if (results[4] == TEST_PASSED) {
        printf(COLOR_GREEN "\n✓ 推荐使用DMABUF模式:\n" COLOR_RESET);
        printf("  优势: 零拷贝，支持跨设备共享\n");
        printf("  适用: 多消费者场景（显示+编码）\n");
        printf("        需要硬件加速的应用\n");
    }

    if (results[3] == TEST_PASSED) {
        printf(COLOR_YELLOW "\n✓ 可使用USERPTR模式:\n" COLOR_RESET);
        printf("  注意: 有一次内存拷贝，性能较差\n");
        printf("  适用: 特殊内存布局需求\n");
        printf("        调试目的\n");
    }

    if (results[2] != TEST_PASSED && results[4] != TEST_PASSED &&
        results[3] != TEST_PASSED) {
        printf(COLOR_RED "\n✗ 未找到支持的缓冲区模式\n" COLOR_RESET);
        printf("  可能的原因:\n");
        printf("  1. 设备不支持视频捕获\n");
        printf("  2. 设备驱动有问题\n");
        printf("  3. 权限不足（尝试sudo运行）\n");
    }

    // 关闭设备
    close(fd);

    printf(COLOR_BLUE "\n测试完成\n" COLOR_RESET);
    return EXIT_SUCCESS;
}