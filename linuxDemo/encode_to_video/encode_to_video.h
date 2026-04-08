#ifndef _ENCODE_VIDEO_H
#define _ENCODE_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif
#include "img_transfer_config.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <stdint.h>

// 线程安全控制宏（默认启用）
#ifndef ENCODE_ENABLE_THREAD_SAFE
#define ENCODE_ENABLE_THREAD_SAFE 1
#endif

// 输出目标最大尝试重连次数
#ifndef ENCODE_OUTPUT_MAX_RECONNECT
#define ENCODE_OUTPUT_MAX_RECONNECT (3)
#endif

//=================== 跨平台线程安全抽象层 ===================
#if ENCODE_ENABLE_THREAD_SAFE
#include <pthread.h>
#define encode_mutex_t pthread_mutex_t
#define encode_cond_t pthread_cond_t
#define encode_mutex_init(m) pthread_mutex_init(m, NULL)
#define encode_mutex_destroy(m) pthread_mutex_destroy(m)
#define encode_mutex_lock(m) pthread_mutex_lock(m)
#define encode_mutex_unlock(m) pthread_mutex_unlock(m)
#define encode_cond_init(c) pthread_cond_init(c, NULL)
#define encode_cond_destroy(c) pthread_cond_destroy(c)
#define encode_cond_signal(c) pthread_cond_signal(c)
#define encode_cond_wait(c, m) pthread_cond_wait(c, m)
#else
// 空类型占位（实际不会使用）
#define encode_mutex_t int
#define encode_cond_t int
#define encode_mutex_init(m) ((void)0)
#define encode_mutex_destroy(m) ((void)0)
#define encode_mutex_lock(m) ((void)0)
#define encode_mutex_unlock(m) ((void)0)
#define encode_cond_init(c) ((void)0)
#define encode_cond_destroy(c) ((void)0)
#define encode_cond_signal(c) ((void)0)
#define encode_cond_wait(c, m) ((void)0)
#endif

typedef struct OutputStream {
    AVCodecContext *enc_ctx; // 编码器上下文
    int64_t next_pts;        // 显示时间辍
    AVFrame *frame;          // 编码器所需的原始数据
    AVPacket *tmp_pkt;       // 接收编码器输出的压缩数据
} OutputStream;

typedef struct OutputTarget {
    AVFormatContext *fmt_ctx; // 目标输出格式上下文
    AVStream *st;             // 目标流
    char name[256];           // 目标文件名或者url
    int64_t start_pts;        // 该目标开始时的编码器PTS（用于相对时间）
    int base_set;             // 0-未设置基准，1-已设置
    int broken;               // 1表示连接断开，需要重连
    int reconnect_attempts;   // 当前已尝试重连次数
} OutputTarget;

// 统计结构体
typedef struct EncoderStats {
    int64_t frame_count;      // 已编码帧数
    int64_t packet_count;     // 已推流包数
    int64_t dropped_count;    // 丢弃包数
    int64_t start_time;       // 起始时间 (微秒)
    int64_t last_time;        // 上次打印时间
    int64_t last_frame_count; // 上次打印时的帧计数
} EncoderStats;

typedef struct EncoderContext {
    OutputStream out_st;     // 输出流编码器相关
    const AVCodec *codec;    // 编码器指针
    enum AVCodecID codec_id; // 编码器类型

    // 已编码包队列
    AVFifoBuffer *packet_queue; // ffmpeg的fifo缓冲区
    encode_mutex_t queue_lock;  // 队列互斥锁
    encode_cond_t queue_cond;   // 队列条件变量
    int encoding_finished;      // 编码结束标志

    // 空闲包池
    AVFifoBuffer *free_packet_queue; // 空闲包队列（存储 AVPacket*）
    encode_mutex_t pool_lock;        // 空闲包池锁

    // 输出目标列表（由推流线程使用）
    OutputTarget *target;        // 输出目标数组，输出格式相关
    int num_targets;             // 目标数量
    encode_mutex_t targets_lock; // 保护目标列表的锁

    // 统计数据
    EncoderStats stats;
    encode_mutex_t stats_lock; // 保护统计数据的锁
} EncoderContext;

/**
 * @brief 初始化编码器
 * @param pctx 编码器上下文
 * @param w 宽
 * @param h 高
 * @param fps 帧率
 * @param thread 编码线程数
 * @param internal_queue_size 内部队列大小
 * @param codec_id 编码器类型
 * @return int 成功返回0 失败返回-1
 */
int encoder_init(EncoderContext **pctx, int w, int h, int fps, int thread,
                 int internal_queue_size, enum AVCodecID codec_id);
/**
 * @brief 添加输出
 * @param ctx 编码器上下文
 * @param filename 文件名/RTMP链接
 * @return int 成功返回0 失败返回-1
 */
int encoder_add_output(EncoderContext *ctx, const char *filename);
/**
 * @brief 移除一个输出目标（文件名或URL）
 * @param ctx 编码器上下文
 * @param filename 要移除的目标名称（与添加时传入的字符串一致）
 * @return 成功返回0，失败（未找到）返回-1
 */
int encoder_remove_output(EncoderContext *ctx, const char *filename);
/**
 * @brief 编码器编码一帧
 * @param ctx 编码器上下文
 * @param img_buf 图像缓冲区，为NULL时启用内部测试图像生成
 * @param img_buf_size 图像大小
 * @return 返回0，需要更多输入；返回1，编码器中没有数据；返回-1，失败
 */
int encoder_frame(EncoderContext *ctx, uint8_t *img_buf, int img_buf_size);
/**
 * @brief 输出编码后的包
 * @param ctx 编码器上下文
 * @return 成功返回0，失败返回-1，空返回1
 */
int encoder_output_packets(EncoderContext *ctx);
/**
 * @brief 重新连接输出目标
 * @param ctx 编码器上下文
 * @return 成功返回0,失败返回-1
 */
int encoder_reconnect_broken(EncoderContext *ctx);
/**
 * @brief 查询编码队列是否为空
 * @param ctx 编码器上下文
 * @return 1 表示队列为空，0 表示队列非空
 */
int encoder_queue_empty(EncoderContext *ctx);
/**
 * @brief 关闭编码器
 * @param ctx 编码器上下文
 */
void encoder_close(EncoderContext *ctx);
/**
 * @brief 打印性能数据
 * @param ctx 编码器上下文
 */
void encoder_print_performance(EncoderContext *ctx);
/**
 * @brief 动态调整 CRF 值（范围 0~51，越小质量越高）内部限制最大码率
 * @param ctx 编码器上下文
 * @param crf_value CRF 值
 * @return int 成功返回0，失败返回-1
 */
int encoder_set_crf(EncoderContext *ctx, int crf_value);
/**
 * @brief 动态调整关键帧间隔（例如每 50 帧一个 I 帧）
 * @param ctx 编码器上下文
 * @param gop_size 关键帧间隔
 * @return int 成功返回0，失败返回-1
 */
int encoder_set_gopsize(EncoderContext *ctx, int gop_size);
/**
 * @brief 动态调整最大码率(bps)
 * @param ctx 编码器上下文
 * @param max_bitrate 最大码率
 * @return int 成功返回0，失败返回-1
 */
int encoder_set_max_bitrate(EncoderContext *ctx, int max_bitrate);
#ifdef __cplusplus
}
#endif

#endif //_ENCODE_VIDEO_H