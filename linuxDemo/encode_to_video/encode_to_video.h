#ifndef _ENCODE_VIDEO_H
#define _ENCODE_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdint.h>
typedef struct OutputStream {
    AVStream *st;            // 视频流
    AVCodecContext *enc_ctx; // 编码器上下文
    int64_t next_pts;        // 显示时间辍
    AVFrame *frame;          // 编码器所需的原始数据
    AVPacket *tmp_pkt;       // 接收编码器输出的压缩数据
} OutputStream;

typedef struct EncoderContext {
    OutputStream out_st;
    AVFormatContext *fmt_ctx;
    const AVCodec *codec;
} EncoderContext;

int encoder_init(EncoderContext **pctx, const char *filename, int w, int h,
                 int fps);
/**
 * @brief
 * @param ctx
 * @param img_buf
 * @param img_buf_size
 * @return 返回0，需要更多输入；返回1，编码器中没有数据；返回-1，失败
 */
int encode_frame(EncoderContext *ctx, uint8_t *img_buf, int img_buf_size);
void encoder_close(EncoderContext *ctx);

#ifdef __cplusplus
}
#endif

#endif //_ENCODE_VIDEO_H