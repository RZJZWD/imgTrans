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
#define ENCODE_ENABLE_THREAD_SAFE 1 // 开启编码器线程安全

#endif //_IMG_TRANSFER_CFG_H_