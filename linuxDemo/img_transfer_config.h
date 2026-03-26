#ifndef _IMG_TRANSFER_CFG_H_
#define _IMG_TRANSFER_CFG_H_

/******dmabuf_manager配置******/
// 设置1时使用malloc作为dmabuf_manager的分配函数，否则默认使用DMABUF
#define DMABUF_ALLOC_MODE 0 // 使用malloc方式分配
// 启用线程安全
#define DMABUF_ENABLE_THREAD_SAFE 1
// 显示资源信息
// 缓冲区状态块宽度，例如 "[2I###]" 缓冲区2号处于空闲状态
#define DMABUF_BUFFER_BLOCK_WIDTH (14)
// 用于打印资源信息的状态简称，必须保证单字符！！！
// 未分配字符
#define DMABUF_MONITOR_UNALLOCATE_CHAR ('U')
// 空闲字符
#define DMABUF_MONITOR_IDLE_CHAR ('I')
// 忙碌字符。表示已使用状态
#define DMABUF_MONITOR_BUSY_CHAR ('B')
// 已使用块填充字符（例如进度条中表示已占用的部分）
#define DMABUF_MONITOR_USED_CHAR ('#')
// 空闲块填充字符（例如进度条中表示未占用的部分）
#define DMABUF_MONITOR_FREE_CHAR ('-')
// 默认每行显示块数（若无法获取终端宽度）
#define DMABUF_MONITOR_DEFAULT_COLS 8

/******display_rga配置******/
// 设置1时开启零拷贝
#define DISPLAY_ENABLE_ZERO_COPY 1

/******encode_to_video配置 */
#define ENCODE_ENABLE_THREAD_SAFE 1     // 开启编码器线程安全
#define ENCODE_OUTPUT_MAX_RECONNECT (3) // 输出目标重新连接尝试次数

/******img_trans默认配置****** */
// 基础
#define CAMERA_WIDTH (640)          // 摄像头采集宽，也是后面所有图像数据的宽
#define CAMERA_HEIGHT (480)         // 摄像头采集高，也是后面所有图像数据的高
#define VIDEO_TARGET_FRAMERATE (15) // 编码视频目标帧率
#define VIDEO_OUTPUT_TARGET (10)    // 输出目标个数
#define LOCAL_DISPLAY (0)           // 默认关闭本地屏幕显示
// 缓冲池
#if DMABUF_ALLOC_MODE == 0
#define CAMERA_INIT_FRAMES (4) // 摄像头初始化内部帧个数
#define POOL_SIZE (40)         // 缓冲池大小
#define CAMERA_QUEUE_SIZE (4)  // 摄像头缓冲队列大小，生产原始图像JPEG
#define RGB_QUEUE_SIZE (3) // rgb数据队列大小，jpeg解码消费原始图像，生产rgb图像
#define YUV_QUEUE_SIZE (3) // yuv420队列大小，将摄像头采集图像转为yuv420p

#elif DMABUF_ALLOC_MODE == 1
#define CAMERA_INIT_FRAMES (2) // 摄像头初始化内部帧个数
#define POOL_SIZE (6)          // 缓冲池大小
#define CAMERA_QUEUE_SIZE (2)  // 摄像头缓冲队列大小，生产原始图像JPEG
#define RGB_QUEUE_SIZE (2) // rgb数据队列大小，jpeg解码消费原始图像，生产rgb图像
#define YUV_QUEUE_SIZE (2) // yuv420队列大小，在jpeg解码时，rgb转yuv420p

#endif
#define VIDEO_FULL_DROPPED (1) // 视频队列满时丢弃队列项

#endif //_IMG_TRANSFER_CFG_H_