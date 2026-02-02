#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
// JPEG库头文件 - 在包含jpeglib.h之前包含stdio.h
#include <jpeglib.h>
// im2d 头文件
#include "RgaUtils.h"
#include "im2d.h"

// 定义 DMA-Heap 路径
#define DMA_HEAP_SYSTEM_PATH "/dev/dma_heap/system"
#define DMA_HEAP_CMA_PATH "/dev/dma_heap/linux,cma"

// 函数声明
int alloc_dmabuf(size_t size, int *dma_fd, void **mapped_addr);
void free_dmabuf(int dma_fd, void *mapped_addr, size_t size);
int init_camera_dmabuf(const char *device, int width, int height, int *v4l2_fd,
                       int *dma_fd, void **buffer, size_t *buffer_size);
int capture_frame_dmabuf(int fd);
int convert_yuyv_to_rgb_with_dmabuf(int yuyv_dma_fd, void *yuyv_data,
                                    int rgb_dma_fd, void *rgb_data, int width,
                                    int height);
int convert_yuyv_to_rgb_software(uint8_t *yuyv_data, uint8_t *rgb_data,
                                 int width, int height);
void save_jpeg(const char *filename, uint8_t *rgb_data, int width, int height,
               int quality);
void save_ppm(const char *filename, uint8_t *rgb_data, int width, int height);

int main(int argc, char *argv[]) {
    // 默认参数
    const char *device = "/dev/video0";
    int width = 640;
    int height = 480;
    const char *output_base = "capture_dmabuf";
    int jpeg_quality = 85;

    // 解析命令行参数
    if (argc > 1)
        device = argv[1];
    if (argc > 3) {
        width = atoi(argv[2]);
        height = atoi(argv[3]);
    }
    if (argc > 4) {
        jpeg_quality = atoi(argv[4]);
    }

    printf("DMA-BUF 摄像头捕获程序 (V4L2 DMABUF导入 + RGA加速)\n");
    printf("摄像头设备: %s\n", device);
    printf("分辨率: %dx%d\n", width, height);
    printf("JPEG质量: %d%%\n", jpeg_quality);

    // 1. 初始化摄像头（使用DMA-BUF）
    int v4l2_fd;
    int yuyv_dma_fd;
    void *yuyv_buffer;
    size_t yuyv_buffer_size;

    if (init_camera_dmabuf(device, width, height, &v4l2_fd, &yuyv_dma_fd,
                           &yuyv_buffer, &yuyv_buffer_size) < 0) {
        fprintf(stderr, "摄像头初始化失败\n");
        return -1;
    }

    printf("YUYV DMA-BUF分配成功: FD=%d, 大小=%zu\n", yuyv_dma_fd,
           yuyv_buffer_size);

    // 2. 捕获一帧
    printf("等待帧...\n");
    if (capture_frame_dmabuf(v4l2_fd) < 0) {
        fprintf(stderr, "捕获帧失败\n");
        free_dmabuf(yuyv_dma_fd, yuyv_buffer, yuyv_buffer_size);
        close(v4l2_fd);
        return -1;
    }

    printf("捕获到帧! 大小: %zu\n", yuyv_buffer_size);

    // 3. 为RGB分配DMA-BUF
    int rgb_dma_fd;
    void *rgb_buffer;
    size_t rgb_buffer_size = width * height * 3;

    if (alloc_dmabuf(rgb_buffer_size, &rgb_dma_fd, &rgb_buffer) < 0) {
        fprintf(stderr, "分配RGB DMA-BUF失败\n");
        free_dmabuf(yuyv_dma_fd, yuyv_buffer, yuyv_buffer_size);
        close(v4l2_fd);
        return -1;
    }

    printf("RGB DMA-BUF分配成功: FD=%d, 大小=%zu\n", rgb_dma_fd,
           rgb_buffer_size);

    // 4. 使用RGA进行颜色转换（DMA-BUF到DMA-BUF）
    printf("使用RGA进行DMA-BUF颜色转换...\n");
    int ret = convert_yuyv_to_rgb_with_dmabuf(
        yuyv_dma_fd, yuyv_buffer, rgb_dma_fd, rgb_buffer, width, height);

    if (ret != 0) {
        printf("RGA DMA-BUF转换失败\n");
        // convert_yuyv_to_rgb_software((uint8_t *)yuyv_buffer,
        //                              (uint8_t *)rgb_buffer, width, height);
    }

    // 5. 保存图像文件
    char filename[256];

    // 保存为PPM
    snprintf(filename, sizeof(filename), "%s.ppm", output_base);
    save_ppm(filename, (uint8_t *)rgb_buffer, width, height);

    // 保存为JPEG
    snprintf(filename, sizeof(filename), "%s.jpg", output_base);
    save_jpeg(filename, (uint8_t *)rgb_buffer, width, height, jpeg_quality);

    // 6. 清理资源
    free_dmabuf(yuyv_dma_fd, yuyv_buffer, yuyv_buffer_size);
    free_dmabuf(rgb_dma_fd, rgb_buffer, rgb_buffer_size);

    // 停止视频流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
    close(v4l2_fd);

    printf("捕获完成!\n");
    return 0;
}

/**
 * 分配DMA-BUF内存
 */
int alloc_dmabuf(size_t size, int *dma_fd, void **mapped_addr) {
    int heap_fd = open(DMA_HEAP_SYSTEM_PATH, O_RDWR);
    if (heap_fd < 0) {
        // 尝试CMA heap
        heap_fd = open(DMA_HEAP_CMA_PATH, O_RDWR);
        if (heap_fd < 0) {
            perror("打开DMA-Heap失败");
            return -1;
        }
    }

    struct dma_heap_allocation_data alloc_data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        perror("DMA-Heap分配失败");
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    *dma_fd = alloc_data.fd;

    // 映射到用户空间
    *mapped_addr =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *dma_fd, 0);
    if (*mapped_addr == MAP_FAILED) {
        perror("映射DMA-BUF失败");
        close(*dma_fd);
        return -1;
    }

    return 0;
}

/**
 * 释放DMA-BUF内存
 */
void free_dmabuf(int dma_fd, void *mapped_addr, size_t size) {
    if (mapped_addr != MAP_FAILED && mapped_addr != NULL) {
        munmap(mapped_addr, size);
    }
    if (dma_fd >= 0) {
        close(dma_fd);
    }
}

/**
 * 初始化摄像头（使用DMA-BUF导入）
 */
int init_camera_dmabuf(const char *device, int width, int height, int *v4l2_fd,
                       int *dma_fd, void **buffer, size_t *buffer_size) {
    // 1. 打开摄像头设备
    *v4l2_fd = open(device, O_RDWR);
    if (*v4l2_fd < 0) {
        perror("打开摄像头失败");
        return -1;
    }

    // 2. 查询设备能力
    struct v4l2_capability cap;
    if (ioctl(*v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("查询设备能力失败");
        close(*v4l2_fd);
        return -1;
    }

    printf("摄像头名称: %s\n", cap.card);
    printf("驱动: %s\n", cap.driver);

    // 检查是否支持DMA-BUF导入
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "设备不支持流式I/O\n");
        close(*v4l2_fd);
        return -1;
    }

    // 3. 设置视频格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(*v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("设置格式失败");
        close(*v4l2_fd);
        return -1;
    }

    printf("实际格式: YUYV\n");
    printf("实际分辨率: %dx%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 4. 计算所需缓冲区大小
    *buffer_size = fmt.fmt.pix.sizeimage;
    if (*buffer_size == 0) {
        *buffer_size = width * height * 2; // YUYV: 2 bytes per pixel
    }

    // 5. 分配DMA-BUF
    if (alloc_dmabuf(*buffer_size, dma_fd, buffer) < 0) {
        fprintf(stderr, "分配DMA-BUF失败\n");
        close(*v4l2_fd);
        return -1;
    }

    // 6. 请求缓冲区（使用DMA-BUF）
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF; // 使用DMA-BUF内存类型

    if (ioctl(*v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("请求DMA-BUF缓冲区失败");
        free_dmabuf(*dma_fd, *buffer, *buffer_size);
        close(*v4l2_fd);
        return -1;
    }

    // 7. 创建并配置缓冲区（使用DMA-BUF FD）
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 0;

    // // 设置DMA-BUF文件描述符
    // struct v4l2_plane planes[1];
    // memset(&planes, 0, sizeof(planes));
    // planes[0].m.fd = *dma_fd;
    // planes[0].length = *buffer_size;
    // planes[0].data_offset = 0;
    // planes[0].bytesused = 0;

    // buf.m.planes = planes;
    // buf.length = 1;
    buf.m.fd = *dma_fd;

    // 8. 将缓冲区加入队列
    if (ioctl(*v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        perror("DMA-BUF缓冲区入队失败");
        free_dmabuf(*dma_fd, *buffer, *buffer_size);
        close(*v4l2_fd);
        return -1;
    }

    // 9. 开始视频流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(*v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("开始流失败");
        free_dmabuf(*dma_fd, *buffer, *buffer_size);
        close(*v4l2_fd);
        return -1;
    }

    printf("摄像头DMA-BUF初始化成功\n");
    return 0;
}

/**
 * 捕获一帧图像（DMA-BUF版本）
 */
int capture_frame_dmabuf(int fd) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 5; // 5秒超时
    tv.tv_usec = 0;

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        printf("等待帧超时或出错\n");
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];

    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;
    buf.length = 1;

    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("获取帧失败");
        return -1;
    }

    // 重新入队缓冲区以便下次捕获
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("重新入队缓冲区失败");
        return -1;
    }

    return 0;
}

/**
 * 使用im2d进行DMA-BUF到DMA-BUF的颜色转换
 */
int convert_yuyv_to_rgb_with_dmabuf(int yuyv_dma_fd, void *yuyv_data,
                                    int rgb_dma_fd, void *rgb_data, int width,
                                    int height) {
    printf("开始DMA-BUF颜色转换...\n");

    rga_buffer_handle_t src_handle = 0, dst_handle = 0;
    im_handle_param_t src_param, dst_param;
    rga_buffer_t src_buf, dst_buf;

    // 设置参数
    src_param.format = RK_FORMAT_YUYV_422;
    src_param.width = width;
    src_param.height = height;
    dst_param.format = RK_FORMAT_RGB_888;
    dst_param.width = width;
    dst_param.height = height;

    // 导入dmabuf
    src_handle = importbuffer_fd(yuyv_dma_fd, &src_param);
    dst_handle = importbuffer_fd(rgb_dma_fd, &dst_param);
    // src_handle = importbuffer_virtualaddr(yuyv_data, &src_param);
    // dst_handle = importbuffer_virtualaddr(rgb_data, &dst_param);
    if (src_handle == 0 || dst_handle == 0) {
        printf("\nrga handle 处理错误，退出处理\n");
        return -1;
    }
    // 导入rgabuffer
    src_buf = wrapbuffer_handle(src_handle, src_param.width, src_param.height,
                                src_param.format);
    dst_buf = wrapbuffer_handle(dst_handle, dst_param.width, dst_param.height,
                                dst_param.format);

    // 图像格式转换
    int ret = imcvtcolor(src_buf, dst_buf, src_param.format, dst_param.format,
                         IM_YUV_TO_RGB_BT601_LIMIT);
    if (ret == IM_STATUS_SUCCESS) {
        printf("\n转换成功\n");
    } else {
        printf("\n转换失败\n");
    }

    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);

    return 0;
}

/**
 * 保存为JPEG格式
 */
void save_jpeg(const char *filename, uint8_t *rgb_data, int width, int height,
               int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *outfile;
    JSAMPROW row_pointer[1];
    int row_stride;

    if ((outfile = fopen(filename, "wb")) == NULL) {
        fprintf(stderr, "无法打开JPEG文件: %s\n", filename);
        return;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb_data[cinfo.next_scanline * row_stride];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);

    printf("已保存JPEG: %s (质量: %d%%)\n", filename, quality);
}

/**
 * 保存为PPM格式
 */
void save_ppm(const char *filename, uint8_t *rgb_data, int width, int height) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("打开PPM文件失败");
        return;
    }

    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb_data, 1, width * height * 3, fp);
    fclose(fp);

    printf("已保存PPM: %s\n", filename);
}