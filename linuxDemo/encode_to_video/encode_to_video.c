#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>

#define STREAM_FRAME_RATE 25              // 25 images/s
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P // default pix_fmt

typedef struct OutputStream {
    AVStream *st;            // 视频流
    AVCodecContext *enc_ctx; // 编码器上下文
    int64_t next_pts;        // 显示时间辍
    AVFrame *frame;          // 编码器所需的原始数据
    AVPacket *tmp_pkt;       // 接收编码器输出的压缩数据
} OutputStream;
static OutputStream video_st;
static const AVOutputFormat *out_fmt;
static AVFormatContext *fmt_ctx;
static const AVCodec *video_codec;
static void log_packet(const AVFormatContext *log_fmt_ctx,
                       const AVPacket *pkt) {
    AVRational *time_base = &log_fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s "
           "stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static void fill_yuv_image(AVFrame *pict, int frame_index, int width,
                           int height) {
    int x, y, i;

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}
static AVFrame *get_video_frame(OutputStream *out_st) {
    AVCodecContext *codec_ctx = out_st->enc_ctx;

    // 确保帧缓冲区没有被其他引用
    if (av_frame_make_writable(out_st->frame) < 0)
        exit(1);
    fill_yuv_image(out_st->frame, out_st->next_pts, codec_ctx->width,
                   codec_ctx->height);
    out_st->frame->pts = out_st->next_pts++;
    return out_st->frame;
}
static AVFrame *alloc_frame(enum AVPixelFormat pix_fmt, int width, int height) {
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = pix_fmt;
    frame->width = width;
    frame->height = height;

    // 申请缓冲区给frame
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "申请frame数据失败\n");
        exit(1);
    }
    return frame;
}

/**
 * @brief 向输出格式上下文添加新流并初始化编码器上下文
 * @param out_st 输出流
 * @param out_fmt_ctx 输出格式上下文
 * @param codec 编码器，二级指针
 * @param codec_id 编码器id
 * @param width 输出流宽
 * @param height 输出流高
 * @param fps 输出流fps
 */
static void add_stream(OutputStream *out_st, AVFormatContext *out_fmt_ctx,
                       const AVCodec **codec, enum AVCodecID codec_id,
                       int width, int height, int fps) {
    AVCodecContext *codec_ctx;
    int i;

    // 寻找编码器
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "找不到编码器 '%s' \n", avcodec_get_name(codec_id));
        exit(1);
    }
    out_st->tmp_pkt = av_packet_alloc();
    if (!out_st->tmp_pkt) {
        fprintf(stderr, "分配AVPacket失败\n");
        exit(1);
    }
    out_st->st = avformat_new_stream(out_fmt_ctx, NULL);
    if (!out_st->st) {
        fprintf(stderr, "分配Stream失败\n");
        exit(1);
    }
    out_st->st->id = out_fmt_ctx->nb_streams - 1;

    codec_ctx = avcodec_alloc_context3(*codec);
    if (!codec_ctx) {
        fprintf(stderr, "分配编码器上下文失败\n");
        exit(1);
    }
    out_st->enc_ctx = codec_ctx;

    if ((*codec)->type == AVMEDIA_TYPE_VIDEO) {
        codec_ctx->codec_id = codec_id;
        codec_ctx->bit_rate = 400000;
        codec_ctx->width = width;
        codec_ctx->height = height;
        out_st->st->time_base = (AVRational){1, fps};
        codec_ctx->time_base = out_st->st->time_base;
        codec_ctx->gop_size = 12;
        codec_ctx->pix_fmt = STREAM_PIX_FMT;

        if (codec_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            codec_ctx->max_b_frames = 2;
        }
        if (codec_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            codec_ctx->mb_decision = 2;
        }
        codec_ctx->thread_count = 1;
    } else {
        printf("编码器类型未指定\n");
        exit(1);
    }
    // 一些编码格式使用全局头信息
    if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**
 * @brief 打开编码器
 * @param out_st 输出流
 * @param out_fmt_ctx 输出格式上下文
 * @param codec 编码器
 */
static void open_video(OutputStream *out_st, AVFormatContext *out_fmt_ctx,
                       const AVCodec *codec) {
    int ret;
    AVCodecContext *codec_ctx = out_st->enc_ctx;

    // 打开编码器
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打开视频编码器失败: %s\n", av_err2str(ret));
        exit(1);
    }

    // 分配编码帧
    out_st->frame =
        alloc_frame(codec_ctx->pix_fmt, codec_ctx->width, codec_ctx->height);
    if (!out_st->frame) {
        fprintf(stderr, "分配video frame失败\n");
        exit(1);
    }

    // 复制编码器参数到流中
    ret = avcodec_parameters_from_context(out_st->st->codecpar, codec_ctx);
    if (ret < 0) {
        fprintf(stderr, "复制stream parameters失败\n");
        exit(1);
    }
}

static int encode_and_write_frame(AVFrame *frame, OutputStream *out_st,
                                  AVFormatContext *out_fmt_ctx) {
    // 向编码器发送帧缓冲区
    int ret;
    ret = avcodec_send_frame(out_st->enc_ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "向编码器 %s 发送帧缓冲区出错 \n", av_err2str(ret));
        exit(1);
    }

    // 循环接受编码后的包 avpacket
    while (ret >= 0) {
        ret = avcodec_receive_packet(out_st->enc_ctx, out_st->tmp_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "帧编码错误: %s\n", av_err2str(ret));
            exit(1);
        }

        // 处理时间基，将包时间戳从编码器时基转为流时基
        av_packet_rescale_ts(out_st->tmp_pkt, out_st->enc_ctx->time_base,
                             out_st->st->time_base);
        out_st->tmp_pkt->stream_index = out_st->st->index;

        // 打印包信息
        log_packet(out_fmt_ctx, out_st->tmp_pkt);
        ret = av_interleaved_write_frame(out_fmt_ctx, out_st->tmp_pkt);
        if (ret < 0) {
            fprintf(stderr, "写入到输出数据包错误: %s\n", av_err2str(ret));
            exit(1);
        }
    }
    return ret == AVERROR_EOF ? 1 : 0;
}
/**
 * @brief 将一帧数据写入编码器，再接受输出的数据包，转换时间基，写入封装器
 * @param out_fmt_ctx 输出格式上下文
 * @param out_st 输出流
 * @return 成功返回0，还有数据
 * 失败返回1，没有数据
 */
static int write_video_frame(AVFormatContext *out_fmt_ctx,
                             OutputStream *out_st) {
    // 获取视频帧
    AVFrame *frame = get_video_frame(out_st);
    if (!frame) {
        // 可能表示错误或结束，此处返回 -1 表示失败
        return -1;
    }
    return encode_and_write_frame(frame, out_st, out_fmt_ctx);
}

int init_encoder(const char *output_file, int w, int h, int fps) {
    if (!fps) {
        fps = STREAM_FRAME_RATE;
    }
    int ret;
    avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, output_file);
    if (!fmt_ctx) {
        // 如果无法猜测，尝试强制为 MP4（或根据实际需求调整）
        avformat_alloc_output_context2(&fmt_ctx, NULL, "mp4", output_file);
        if (!fmt_ctx) {
            fprintf(stderr, "Could not create output context\n");
            return -1;
        }
    }
    out_fmt = fmt_ctx->oformat;

    // 添加视频流，使用 H.264 编码器（x264 是其实现）
    add_stream(&video_st, fmt_ctx, &video_codec, AV_CODEC_ID_H264, w, h, fps);

    // 打开视频编码器
    open_video(&video_st, fmt_ctx, video_codec);

    // 打印输出格式信息
    av_dump_format(fmt_ctx, 0, output_file, 1);

    // 打开输出文件（如果不是网络流）
    if (!(out_fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx->pb, output_file, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "无法打开输出文件 '%s': %s\n", output_file,
                    av_err2str(ret));
            return ret;
        }
    }

    // 写入文件头
    ret = avformat_write_header(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "写入头失败: %s\n", av_err2str(ret));
        return ret;
    }

    return 0;
}
int encode_frame(uint8_t *img_buf, int img_buf_size) {
    if (img_buf == NULL) {
        // 使用内部生成模式
        return write_video_frame(fmt_ctx, &video_st);
    } else {
        // 使用外部图像数据
        AVFrame *frame = video_st.frame;
        AVCodecContext *c = video_st.enc_ctx;

        // 确保帧可写
        if (av_frame_make_writable(frame) < 0) {
            fprintf(stderr, "Frame not writable\n");
            return -1;
        }

        // 假设外部数据为 YUV420P 平面连续存放（Y, U, V）
        int y_size = c->width * c->height;
        int uv_size = y_size / 4;
        if (img_buf_size < y_size + 2 * uv_size) {
            fprintf(stderr, "Image buffer too small\n");
            return -1;
        }
        memcpy(frame->data[0], img_buf, y_size);
        memcpy(frame->data[1], img_buf + y_size, uv_size);
        memcpy(frame->data[2], img_buf + y_size + uv_size, uv_size);

        // 设置 PTS 并递增
        frame->pts = video_st.next_pts++;

        // 调用公共编码写入函数
        return encode_and_write_frame(frame, &video_st, fmt_ctx);
    }
}
void close_encoder() {
    if (video_st.enc_ctx) {
        avcodec_send_frame(video_st.enc_ctx, NULL);
        AVPacket *pkt = video_st.tmp_pkt;
        int ret;
        while (ret >= 0) {
            ret = avcodec_receive_packet(video_st.enc_ctx, pkt);
            if (ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                fprintf(stderr, "刷新编码器失败: %s\n", av_err2str(ret));
                break;
            }
            // 转换时间基并写入
            av_packet_rescale_ts(pkt, video_st.enc_ctx->time_base,
                                 video_st.st->time_base);
            pkt->stream_index = video_st.st->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
        }
    }
    // 写入文件尾
    av_write_trailer(fmt_ctx);

    // 关闭IO
    if (!(out_fmt->flags & AVFMT_NOFILE)) {
        avio_closep(&fmt_ctx->pb);
    }

    // 释放资源
    avcodec_free_context(&video_st.enc_ctx);
    av_frame_free(&video_st.frame);
    av_packet_free(&video_st.tmp_pkt);
    avformat_free_context(fmt_ctx);
}