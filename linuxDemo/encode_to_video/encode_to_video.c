#include "encode_to_video.h"
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_FRAME_RATE 25              // 25 images/s
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P // default pix_fmt

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
        return NULL;
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
        return NULL;
    }
    return frame;
}

/**
 * @brief 设置编码器上下文
 * @param out_st 输出流
 * @param codec 编码器，二级指针
 * @param codec_id 编码器id
 * @param width 输出流宽
 * @param height 输出流高
 * @param fps 输出流fps
 * @param thread 编码线程数
 * @return 成功返回0 失败返回-1
 */
static int set_codec(OutputStream *out_st, const AVCodec **codec,
                     enum AVCodecID codec_id, int width, int height, int fps,
                     int thread) {
    AVCodecContext *codec_ctx;
    int i;

    // 寻找编码器
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "找不到编码器 '%s' \n", avcodec_get_name(codec_id));
        return -1;
    }
    out_st->tmp_pkt = av_packet_alloc();
    if (!out_st->tmp_pkt) {
        fprintf(stderr, "分配AVPacket失败\n");
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(*codec);
    if (!codec_ctx) {
        fprintf(stderr, "分配编码器上下文失败\n");
        // 释放avpacket
        av_packet_free(&out_st->tmp_pkt);
        return -1;
    }
    out_st->enc_ctx = codec_ctx;

    if ((*codec)->type == AVMEDIA_TYPE_VIDEO) {
        codec_ctx->codec_id = codec_id;
        codec_ctx->bit_rate = 800000;
        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->time_base = (AVRational){1, fps};
        codec_ctx->gop_size = fps * 4;
        codec_ctx->max_b_frames = 0;
        codec_ctx->pix_fmt = STREAM_PIX_FMT;

        if (codec_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            codec_ctx->max_b_frames = 2;
        }
        if (codec_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            codec_ctx->mb_decision = 2;
        }
        codec_ctx->thread_count = thread;
        codec_ctx->qmin = 18; // 最低量化值（原15，可降低画质上限）
        codec_ctx->qmax = 35; // 最高量化值（原45，压缩过大）
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else {
        printf("编码器类型未指定\n");
        // 释放avpacket
        av_packet_free(&out_st->tmp_pkt);
        // 释放编码器上下文
        avcodec_free_context(&out_st->enc_ctx);
        return -1;
    }

    return 0;
}
/**
 * @brief 打开编码器
 * @param out_st 输出流
 * @param out_fmt_ctx 输出格式上下文
 * @param codec 编码器
 * @return 成功返回0 失败返回-1
 */
static int open_codec(OutputStream *out_st, const AVCodec *codec) {
    int ret;
    AVCodecContext *codec_ctx = out_st->enc_ctx;
    AVDictionary *opts = NULL;

    // 嵌入式设备优化参数
    // 将预设从 ultrafast 改为 superfast（速度尚可，画质提升明显）
    av_dict_set(&opts, "preset", "superfast", 0);
    // av_dict_set(&opts, "preset", "ultrafast", 0); // 最快预设
    av_dict_set(&opts, "tune", "zerolatency", 0); // 零延迟
    av_dict_set(&opts, "profile", "baseline", 0); // 最简档次
    // av_dict_set(&opts, "crf", "28", 0); //
    // 提高CRF值，不适合rtmp的恒定码率要求
    av_dict_set(&opts, "me_method", "dia", 0); // 最简运动搜索
    // 适当提高亚像素精度（subq）从1到2，画质提升且速度影响不大
    av_dict_set(&opts, "subq", "2", 0);
    av_dict_set(&opts, "refs", "1", 0);          // 最少参考帧
    av_dict_set(&opts, "partitions", "none", 0); // 禁用分区分析

    // 新增的极端优化选项（针对 ARM 低性能设备）
    av_dict_set(&opts, "rc_lookahead", "0", 0);   // 关闭码率控制前瞻
    av_dict_set(&opts, "sync_lookahead", "0", 0); // 关闭线程前瞻
    av_dict_set(&opts, "me_range", "4", 0);       // 运动搜索范围最小
    av_dict_set(&opts, "trellis", "0", 0);        // 关闭 trellis 量化
    av_dict_set(&opts, "no_dct_decimate", "1",
                0);                           // 不丢弃 DCT 系数（降低分析）
    av_dict_set(&opts, "scenecut", "0", 0);   // 关闭场景切换检测
    av_dict_set(&opts, "fast_pskip", "1", 0); // 启用快速 P 帧跳过
    av_dict_set(&opts, "dct8x8", "0", 0);     // 禁用 8x8 DCT
    av_dict_set(&opts, "weightp", "0", 0);    // 关闭加权预测
    av_dict_set(&opts, "aq-mode", "0", 0);    // 关闭自适应量化
    av_dict_set(&opts, "mbtree", "0", 0);     // 关闭宏块树码率控制

    // 打开编码器
    ret = avcodec_open2(codec_ctx, codec, &opts);
    if (ret < 0) {
        fprintf(stderr, "打开视频编码器失败: %s\n", av_err2str(ret));
        return -1;
    }

    // 分配编码帧
    out_st->frame =
        alloc_frame(codec_ctx->pix_fmt, codec_ctx->width, codec_ctx->height);
    if (!out_st->frame) {
        fprintf(stderr, "分配video frame失败\n");
        // 关闭编码器
        avcodec_free_context(&out_st->enc_ctx);
        return -1;
    }

    return 0;
}
/**
 * @brief 编码并向包写入一帧
 * @param frame 帧数据
 * @param out_st 输出流
 * @param out_fmt_ctx 输出格式上下文
 * @return 返回0，需要更多输入;返回1，编码器中没有数据;返回-1，失败
 */
static int encode_and_write_frame(AVFrame *frame, EncoderContext *ctx) {
    // 向编码器发送帧缓冲区
    OutputStream *out_st = &ctx->out_st;
    int ret = avcodec_send_frame(out_st->enc_ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "向编码器 %s 发送帧缓冲区出错 \n", av_err2str(ret));
        return -1;
    }

    // 循环接受编码后的包 avpacket
    while (ret >= 0) {
        ret = avcodec_receive_packet(out_st->enc_ctx, out_st->tmp_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "帧编码错误: %s\n", av_err2str(ret));
            return -1;
        }
        for (int i = 0; i < ctx->num_targets; i++) {
            OutputTarget *target = &ctx->target[i];
            AVPacket *pkt_clone = av_packet_clone(out_st->tmp_pkt);
            if (!pkt_clone)
                return -1;

            // // 处理时间基，将包时间戳从编码器时基转为流时基
            // av_packet_rescale_ts(out_st->tmp_pkt, out_st->enc_ctx->time_base,
            //                      target->st->time_base);
            // out_st->tmp_pkt->stream_index = target->st->index;
            // // 打印包信息
            // log_packet(target->fmt_ctx, out_st->tmp_pkt);

            // 计算相对时间戳
            pkt_clone->pts -= target->start_pts;
            pkt_clone->dts -= target->start_pts;
            // 处理时间基，将克隆包时间戳从编码器时基转为流时基
            av_packet_rescale_ts(pkt_clone, out_st->enc_ctx->time_base,
                                 target->st->time_base);
            pkt_clone->stream_index = target->st->index;
            // log_packet(target->fmt_ctx, pkt_clone); // 打印克隆后的信息

            int write_ret =
                av_interleaved_write_frame(target->fmt_ctx, pkt_clone);
            av_packet_free(&pkt_clone);
            if (write_ret < 0) {
                fprintf(stderr, "写入到输出数据包错误: %s\n",
                        av_err2str(write_ret));
                return -1;
            }
        }
    }
    // 如果因为AVERROR_EOF（编码器完全刷新）跳出循环返回1,表示编码器中没有数据了；否则编码器还需要输入才可以输出
    return ret == AVERROR_EOF ? 1 : 0;
}
/**
 * @brief 将一帧数据写入编码器，再接受输出的数据包，转换时间基，写入封装器
 * @param out_fmt_ctx 输出格式上下文
 * @param out_st 输出流
 * @return 返回0，需要更多输入;返回1，编码器中没有数据;返回-1，失败
 */
static int write_video_frame(EncoderContext *ctx) {
    // 获取视频帧
    AVFrame *frame = get_video_frame(&ctx->out_st);
    if (!frame) {
        // 可能表示错误或结束，此处返回 -1 表示失败
        printf("内部生成图像，获取帧失败\n");
        return 1;
    }
    return encode_and_write_frame(frame, ctx);
}

int encoder_init(EncoderContext **pctx, int w, int h, int fps, int thread) {
    if (fps == 0) {
        fps = STREAM_FRAME_RATE;
    }
    // 初始化EncoderContext
    EncoderContext *ctx = av_mallocz(sizeof(EncoderContext));
    if (!ctx)
        return -1;

    OutputStream *out_st = &ctx->out_st;
    const AVCodec **codec_ptr = &ctx->codec;
    int ret;

    // 添加视频流
    printf("[设置编码器] ");
    ret = set_codec(out_st, codec_ptr, AV_CODEC_ID_H264, w, h, fps, thread);
    if (ret < 0) {
        fprintf(stderr, "设置编码器失败\n");
        goto fail;
    }

    // 打开视频编码器（使用 out_st 和 ctx->fmt_ctx）
    printf("[开启编码器] ");
    ret = open_codec(out_st, ctx->codec);
    if (ret < 0) {
        fprintf(stderr, "开启编码器失败\n");
        goto fail;
    }
    // 初始化网络
    avformat_network_init();

    *pctx = ctx;
    return 0;

fail:
    if (out_st->tmp_pkt)
        av_packet_free(&out_st->tmp_pkt);
    if (out_st->enc_ctx)
        avcodec_free_context(&out_st->enc_ctx);
    if (out_st->frame)
        av_frame_free(&out_st->frame);
    av_free(ctx);
    return -1;
}
int encoder_add_output(EncoderContext *ctx, const char *filename) {
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    // 判断是否为rtmp
    const char *format = (strncmp(filename, "rtmp://", 7) == 0) ? "flv" : NULL;
    // 分配输出上下文（直接取地址）
    ret = avformat_alloc_output_context2(&fmt_ctx, NULL, format, filename);
    if (ret < 0 || !fmt_ctx) {
        fprintf(stderr, "创建输出上下文失败\n");
        return -1;
    }
    // // 一些编码格式使用全局头信息,如mp4
    // if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    //     ctx->out_st.enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 创建流并复制编码器参数
    AVStream *st = avformat_new_stream(fmt_ctx, NULL);
    if (!st) {
        fprintf(stderr, "分配Stream失败\n");
        avformat_free_context(fmt_ctx);
        return -1;
    }
    ret = avcodec_parameters_from_context(st->codecpar, ctx->out_st.enc_ctx);
    if (ret < 0) {
        fprintf(stderr, "复制stream parameters失败\n");
        // 关闭编码器
        avformat_free_context(fmt_ctx);
        return -1;
    }
    st->time_base = ctx->out_st.enc_ctx->time_base; // 与编码器时基一致

    // 打印输出格式信息
    av_dump_format(fmt_ctx, 0, filename, 1);

    // 打开输出文件
    // printf("[开启输出文件] ");
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "无法打开输出文件 '%s': %s\n", filename,
                    av_err2str(ret));
            avformat_free_context(fmt_ctx);
            return -1;
        }
    }
    // 写入文件头
    // printf("[写入输出文件头] ");
    ret = avformat_write_header(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "写入头失败: %s\n", av_err2str(ret));
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt_ctx->pb);
        }
        avformat_free_context(fmt_ctx);
        return -1;
    }

    // 将新输出目标放入动态数组
    OutputTarget *new_target = av_realloc_array(
        ctx->target, ctx->num_targets + 1, sizeof(OutputTarget));
    if (!new_target) {
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        return -1;
    }
    ctx->target = new_target;
    OutputTarget *target = &ctx->target[ctx->num_targets];
    target->fmt_ctx = fmt_ctx;
    target->st = st;
    av_strlcpy(target->name, filename, sizeof(target->name));
    ctx->num_targets++;

    // 设置起始pts，本地文件从当前编码器pts开始，使得文件从0开始，rtmp推流保持连续时间
    if (strncmp(filename, "rtmp://", 7) == 0) {
        target->start_pts = 0; // 推流保持连续时间，不重置
    } else {
        target->start_pts = ctx->out_st.next_pts; // 本地文件从0开始
    }
    printf("添加输出目标：%s\n", filename);
    return 0;
}
int encoder_remove_output(EncoderContext *ctx, const char *filename) {
    if (!ctx || !filename)
        return -1;

    for (int i = 0; i < ctx->num_targets; i++) {
        if (strcmp(ctx->target[i].name, filename) == 0) {
            OutputTarget *target = &ctx->target[i];
            // 写入文件尾
            av_write_trailer(target->fmt_ctx);
            // 关闭IO
            if (!(target->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&target->fmt_ctx->pb);
            }
            // 释放封装上下文
            avformat_free_context(target->fmt_ctx);

            // 移动数组元素，最少保留一个输出流
            if (i < (ctx->num_targets - 1)) {
                memmove(&ctx->target[i], &ctx->target[i + 1],
                        (ctx->num_targets - i - 1) * sizeof(OutputTarget));
            }
            ctx->num_targets--;

            // 缩小数组
            if (ctx->num_targets == 0) {
                av_freep(&ctx->target);
            } else {
                OutputTarget *new_target = av_realloc_array(
                    ctx->target, ctx->num_targets, sizeof(OutputTarget));
                if (new_target) {
                    ctx->target = new_target;
                }
            }
            printf("移除输出目标：%s\n", filename);
            return 0;
        }
    }
    fprintf(stderr, "未找到输出目标: %s\n", filename);
    return -1;
}
int encode_frame(EncoderContext *ctx, uint8_t *img_buf, int img_buf_size) {
    OutputStream *out_st = &(ctx)->out_st;
    if (img_buf == NULL) {
        // 使用内部生成模式
        return write_video_frame(ctx);
    } else {
        // 使用外部图像数据
        AVFrame *frame = out_st->frame;
        AVCodecContext *c = out_st->enc_ctx;

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
        frame->pts = out_st->next_pts++;

        // 调用公共编码写入函数
        return encode_and_write_frame(frame, ctx);
    }
}
void encoder_close(EncoderContext *ctx) {
    if (!ctx)
        return;
    OutputStream *out_st = &ctx->out_st; // 使用指针

    // 刷新编码器：发送 NULL 帧，取出所有剩余包
    if (out_st->enc_ctx) {
        int ret = encode_and_write_frame(NULL, ctx);
        if (ret < 0) {
            fprintf(stderr, "刷新编码器失败\n");
        }
    }
    // 关闭所以输出目标
    for (int i = 0; i < ctx->num_targets; i++) {
        OutputTarget *target = &ctx->target[i];
        // 写入文件尾
        av_write_trailer(target->fmt_ctx);
        // 关闭IO
        if (!(target->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&target->fmt_ctx->pb);
        }
        avformat_free_context(target->fmt_ctx);
    }

    // 释放资源
    avcodec_free_context(&out_st->enc_ctx);
    av_frame_free(&out_st->frame);
    av_packet_free(&out_st->tmp_pkt);
    av_free(ctx->target);
    av_free(ctx);
    avformat_network_deinit();
}