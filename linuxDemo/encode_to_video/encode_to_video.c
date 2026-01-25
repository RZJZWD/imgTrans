#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
// 全局上下文
AVFormatContext *fmt_ctx = NULL;
AVCodecContext *enc_ctx = NULL;
AVCodecContext *dec_ctx = NULL;
AVStream *stream = NULL;
struct SwsContext *sws_ctx = NULL;
int frame_count = 0;
int width = 0;
int height = 0;

// 初始化编码器 todo:修改
int init_encoder(const char *output_file, int w, int h, int fps) {
    // 重置全局变量
    fmt_ctx = NULL;
    enc_ctx = NULL;
    dec_ctx = NULL;
    stream = NULL;
    sws_ctx = NULL;
    frame_count = 0;
    width = w;
    height = h;

    // 注册所有组件
    avformat_network_init();

    // 1.创建输出格式上下文
    if (avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, output_file) < 0) {
        return -1;
    }
    // 2. 查找并配置编码器 - 使用软件编码器而不是硬件编码器
    const AVCodec *enc_codec =
        avcodec_find_encoder(AV_CODEC_ID_MPEG4); // 改用MPEG-4软件编码器
    if (!enc_codec) {
        // 如果找不到MPEG-4，尝试其他软件编码器
        enc_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!enc_codec) {
            enc_codec =
                avcodec_find_encoder(AV_CODEC_ID_FLV1); // Flash Video编码器
            if (!enc_codec) {
                fprintf(stderr, "找不到合适的软件编码器\n");
                return -1;
            }
        }
    }
    // 打印使用的编码器名称以便调试
    printf("使用编码器: %s\n", enc_codec->name);
    stream = avformat_new_stream(fmt_ctx, enc_codec);
    if (!stream) {
        return -1;
    }
    enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!enc_ctx) {
        return -1;
    }
    // 设置编码参数
    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->time_base = (AVRational){1, fps};
    enc_ctx->framerate = (AVRational){fps, 1};
    enc_ctx->gop_size = 30;
    enc_ctx->max_b_frames = 1;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->bit_rate = 2000000; // 4 Mbps，提高比特率减少跳帧

    // 如果是H.264编码器，尝试设置软件编码参数
    if (enc_codec->id == AV_CODEC_ID_H264) {
        av_opt_set(enc_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(enc_ctx->priv_data, "profile", "baseline", 0);
    }
    // 打开编码器
    if (avcodec_open2(enc_ctx, enc_codec, NULL) < 0)
        return -1;
    // 关联编码器参数到流
    if (avcodec_parameters_from_context(stream->codecpar, enc_ctx) < 0)
        return -1;

    // 3.创建mjpeg解码器
    const AVCodec *dec_codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!dec_codec)
        return -1;
    dec_ctx = avcodec_alloc_context3(dec_codec);
    if (!dec_ctx)
        return -1;
    if (avcodec_open2(dec_ctx, dec_codec, NULL) < 0)
        return -1;

    // 4.初始化图像转换器jpeg转yuv
    sws_ctx =
        sws_getContext(width, height, AV_PIX_FMT_YUV422P, width, height,
                       AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx)
        return -1;

    // 5.打开输出文件
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0)
            return -1;
    }

    // 6.写入文件头
    if (avformat_write_header(fmt_ctx, NULL) < 0)
        return -1;

    return 0;
}
int encode_frame(uint8_t *jpeg_buf, int buf_size) {
    // 处理刷新
    if (jpeg_buf == NULL) {
        avcodec_send_frame(enc_ctx, NULL);
    } else {
        // === 新增：复制 JPEG 数据到本地缓冲区 ===
        uint8_t *local_buf = av_malloc(buf_size);
        if (!local_buf) {
            return -1; // 内存分配失败
        }
        memcpy(local_buf, jpeg_buf, buf_size);
        // 1.准备mjpeg数据包
        AVPacket *dec_pkt = av_packet_alloc();
        dec_pkt->data = local_buf;
        dec_pkt->size = buf_size;

        // 2.发送到mjpeg解码器
        if (avcodec_send_packet(dec_ctx, dec_pkt) < 0) {
            av_free(local_buf); // 释放本地缓冲区
            av_packet_free(&dec_pkt);
            return -1;
        }

        // 3.接受解码后的帧
        AVFrame *frame = av_frame_alloc();
        if (avcodec_receive_frame(dec_ctx, frame) < 0) {
            av_frame_free(&frame);
            av_free(local_buf); // 释放本地缓冲区
            av_packet_free(&dec_pkt);
            return -1;
        }

        // 4.转换颜色空间
        AVFrame *yuv_frame = av_frame_alloc();
        yuv_frame->format = enc_ctx->pix_fmt;
        yuv_frame->width = enc_ctx->width;
        yuv_frame->height = enc_ctx->height;
        av_frame_get_buffer(yuv_frame, 0);

        sws_scale(sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
                  0, height, yuv_frame->data, yuv_frame->linesize);

        // 5.设置时间辍
        yuv_frame->pts = frame_count++;

        // 6.发送到h.264编码器
        if (avcodec_send_frame(enc_ctx, yuv_frame) < 0) {
            av_frame_free(&frame);
            av_packet_free(&dec_pkt);
            av_free(local_buf); // 释放本地缓冲区
            av_frame_free(&yuv_frame);
            return -1;
        }
        // 清理资源
        av_frame_free(&frame);
        av_frame_free(&yuv_frame);
        av_packet_free(&dec_pkt);
        av_free(local_buf); // 释放本地缓冲区
    }

    // 7.接受编码后的数据包
    AVPacket *enc_pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
        // 设置流索引和时间基
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, stream->time_base);
        enc_pkt->stream_index = stream->index;

        // 写入输出文件
        if (av_interleaved_write_frame(fmt_ctx, enc_pkt) < 0) {
            av_packet_free(&enc_pkt);
            return -1;
        }

        av_packet_unref(enc_pkt);
    }
    av_packet_free(&enc_pkt);

    return 0;
}

// 关闭编码器并释放资源
void close_encoder() {
    // 刷新编码器缓冲区
    encode_frame(NULL, 0);

    // 写入文件尾
    if (fmt_ctx) {
        av_write_trailer(fmt_ctx);
    }

    // 释放资源
    if (sws_ctx)
        sws_freeContext(sws_ctx);
    if (dec_ctx)
        avcodec_free_context(&dec_ctx);
    if (enc_ctx)
        avcodec_free_context(&enc_ctx);

    if (fmt_ctx && !(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmt_ctx->pb);
    }

    if (fmt_ctx)
        avformat_free_context(fmt_ctx);

    // 重置全局变量
    fmt_ctx = NULL;
    enc_ctx = NULL;
    dec_ctx = NULL;
    stream = NULL;
    sws_ctx = NULL;
    frame_count = 0;
    width = 0;
    height = 0;
}