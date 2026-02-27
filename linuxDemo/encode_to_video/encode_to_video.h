#ifndef _ENCODE_VIDEO_H
#define _ENCODE_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdint.h>

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
} OutputTarget;
typedef struct EncoderContext {
    OutputStream out_st;  // 输出流编码器相关
    OutputTarget *target; // 输出目标数组，输出格式相关
    const AVCodec *codec; // 编码器指针
    int num_targets;      // 目标数量
} EncoderContext;

/**
 * @brief 初始化编码器
 * @param pctx 编码器上下文
 * @param w 宽
 * @param h 高
 * @param fps 帧率
 * @return int 成功返回0 失败返回-1
 */
int encoder_init(EncoderContext **pctx, int w, int h, int fps);
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
int encode_frame(EncoderContext *ctx, uint8_t *img_buf, int img_buf_size);
/**
 * @brief 关闭编码器
 * @param ctx 编码器上下文
 */
void encoder_close(EncoderContext *ctx);

#ifdef __cplusplus
}
#endif

#endif //_ENCODE_VIDEO_H