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
        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->time_base = (AVRational){1, fps};

        if (codec_id == AV_CODEC_ID_MJPEG) {
            // MJPEG 特定设置
            codec_ctx->pix_fmt = AV_PIX_FMT_YUVJ422P;  // 常用格式
            codec_ctx->color_range = AVCOL_RANGE_JPEG; // 全范围 (pc)
            codec_ctx->color_primaries =
                AVCOL_PRI_BT470BG;                     // 根据摄像头输出设置
            codec_ctx->colorspace = AVCOL_SPC_BT470BG; // 色彩空间
            codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            codec_ctx->strict_std_compliance = FF_COMPLIANCE_NORMAL;
            // 质量控制：使用全局质量参数（范围 2-31，越小越好）
            codec_ctx->global_quality = 10; // 可根据需求调整
            // 不使用 B 帧、GOP 等
            codec_ctx->gop_size = 0;
            codec_ctx->max_b_frames = 0;
            codec_ctx->has_b_frames = 0;
            // 线程数设为 1（MJPEG 通常单线程）
            codec_ctx->thread_count = 1;
            // 关闭不必要标志
            // codec_ctx->flags &= ~AV_CODEC_FLAG_LOW_DELAY;
        } else {
            // 原有其他视频编码器的设置
            codec_ctx->bit_rate = 800000;
            codec_ctx->gop_size = fps * 4;
            codec_ctx->max_b_frames = 2;
            codec_ctx->has_b_frames = 2;
            codec_ctx->pix_fmt = STREAM_PIX_FMT;

            if (codec_id == AV_CODEC_ID_MPEG2VIDEO) {
                codec_ctx->max_b_frames = 2;
            }
            if (codec_id == AV_CODEC_ID_MPEG1VIDEO) {
                codec_ctx->mb_decision = 2;
            }
            codec_ctx->thread_count = (thread > 0) ? thread : av_cpu_count();
            codec_ctx->thread_type = FF_THREAD_SLICE;
            codec_ctx->qmin = 18;
            codec_ctx->qmax = 35;
            codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        }
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

    // 根据编码器类型设置私有选项
    if (codec_ctx->codec_id == AV_CODEC_ID_H264) {
        // 嵌入式设备优化参数
        // 将预设从 ultrafast 改为 superfast（速度尚可，画质提升明显）
        // av_dict_set(&opts, "preset", "superfast", 0);
        av_dict_set(&opts, "preset", "ultrafast", 0); // 最快预设
        av_dict_set(&opts, "tune", "zerolatency", 0); // 零延迟
        av_dict_set(&opts, "profile", "baseline", 0); // 最简档次

        // // 提高CRF值，不适合rtmp的恒定码率要求
        // // av_dict_set(&opts, "crf", "28", 0);
        // av_dict_set(&opts, "me_method", "dia", 0); // 最简运动搜索
        // // 适当提高亚像素精度（subq）从1到2，画质提升且速度影响不大
        // av_dict_set(&opts, "subq", "2", 0);
        // av_dict_set(&opts, "refs", "1", 0);          // 最少参考帧
        // av_dict_set(&opts, "partitions", "none", 0); // 禁用分区分析

        // // 新增的极端优化选项（针对 ARM 低性能设备）
        // av_dict_set(&opts, "rc_lookahead", "0", 0);   // 关闭码率控制前瞻
        // av_dict_set(&opts, "sync_lookahead", "0", 0); // 关闭线程前瞻
        // av_dict_set(&opts, "me_range", "4", 0);       // 运动搜索范围最小
        // av_dict_set(&opts, "trellis", "0", 0);        // 关闭 trellis 量化
        // av_dict_set(&opts, "no_dct_decimate", "1",
        //             0);                               // 不丢弃 DCT
        //             系数（降低分析）
        // av_dict_set(&opts, "sliced-threads", "1", 0); // 启用切片线程模式
        // av_dict_set(&opts, "slices", "4", 0);         // 显式设置切片数为4
        // av_dict_set(&opts, "scenecut", "0", 0);       // 关闭场景切换检测
        // av_dict_set(&opts, "fast_pskip", "1", 0);     // 启用快速 P 帧跳过
        // av_dict_set(&opts, "dct8x8", "0", 0);         // 禁用 8x8 DCT
        // av_dict_set(&opts, "weightp", "0", 0);        // 关闭加权预测
        // av_dict_set(&opts, "aq-mode", "0", 0);        // 关闭自适应量化
        // av_dict_set(&opts, "mbtree", "0", 0);         // 关闭宏块树码率控制
    }

    // 打开编码器
    ret = avcodec_open2(codec_ctx, codec, &opts);
    if (ret < 0) {
        fprintf(stderr, "打开视频编码器失败: %s\n", av_err2str(ret));
        return -1;
    }

    // 仅当不是 MJPEG 时才分配帧（MJPEG 直推不需要帧缓冲区
    if (codec_ctx->codec_id != AV_CODEC_ID_MJPEG) {
        // 分配编码帧
        out_st->frame = alloc_frame(codec_ctx->pix_fmt, codec_ctx->width,
                                    codec_ctx->height);
        if (!out_st->frame) {
            fprintf(stderr, "分配video frame失败\n");
            // 关闭编码器
            avcodec_free_context(&out_st->enc_ctx);
            return -1;
        }
    } else {
        out_st->frame = NULL;
    }

    return 0;
}
/**
 * @brief 从空闲池获取一个 AVPacket 结构，若池空则动态分配
 * @param ctx 编码器上下文
 * @return 成功返回包指针，失败返回 NULL
 */
static AVPacket *encoder_get_packet(EncoderContext *ctx) {
    AVPacket *pkt = NULL;
    encode_mutex_lock(&ctx->pool_lock);
    if (av_fifo_size(ctx->free_packet_queue) > 0) {
        av_fifo_generic_read(ctx->free_packet_queue, &pkt, sizeof(AVPacket *),
                             NULL);
    }
    encode_mutex_unlock(&ctx->pool_lock);
    if (!pkt) {
        pkt = av_packet_alloc();
        if (!pkt) {
            fprintf(stderr, "无法分配包\n");
        }
    }
    return pkt;
}
/**
 * @brief 将编码后的包放入内部编码队列，内部会自行处理编码队列满时的丢帧
 * @param ctx 编码上下文
 * @param pkt 编码后的新包
 * @return 0入队成功 -1失败
 */
static int enqueue_avpacket(EncoderContext *ctx, AVPacket *new_pkt) {
    if (!new_pkt)
        return -1;

    encode_mutex_lock(&ctx->queue_lock);
    // 检查队列是否有空间
    if (av_fifo_space(ctx->packet_queue) < sizeof(AVPacket *)) {
        // 队列已满
        AVPacket *old_pkt;

        // 先尝试扩大队列容量（例如增加10个指针大小）
        size_t current_size = av_fifo_size(ctx->packet_queue);
        size_t new_size = current_size + 10 * sizeof(AVPacket *);
        if (av_fifo_realloc2(ctx->packet_queue, new_size) >= 0) {
            // 扩大成功，无需丢包，直接跳出，然后将新包入队
            // 注意：av_fifo_realloc2 会保留原有数据
        } else {
            // 扩大失败，必须丢弃一个包
            // 读取最旧的包（但不移除，先 peek 判断是否为关键帧）
            // 注意：FFmpeg 没有直接 peek 的 API，这里先读取再根据情况放回
            av_fifo_generic_read(ctx->packet_queue, &old_pkt,
                                 sizeof(AVPacket *), NULL);
            if (old_pkt->flags & AV_PKT_FLAG_KEY) {
                // 最旧的包是关键帧，不能丢弃
                // 将其放回队列（此时队列已有一个空位）
                av_fifo_generic_write(ctx->packet_queue, &old_pkt,
                                      sizeof(AVPacket *), NULL);

                // 队列重新变满，新包无法入队，只能丢弃新包
                // 丢弃新包（归还空闲池）
                av_packet_free(&new_pkt);
            } else {
                // 非关键帧，直接释放
                av_packet_free(&old_pkt);
                // 此时队列空出一个位置，可以继续入队新包
            }
            // 如果扩充队列失败，无论如何都会丢一帧，旧帧是关键帧就丢新帧，不是关键帧就丢旧帧
            // 丢帧计数
            encode_mutex_lock(&ctx->stats_lock);
            ctx->stats.dropped_count++;
            encode_mutex_unlock(&ctx->stats_lock);
        }
    }
    // 如果 new_pkt 未被丢弃，则入队
    if (new_pkt) {
        av_fifo_generic_write(ctx->packet_queue, &new_pkt, sizeof(AVPacket *),
                              NULL);
        encode_cond_signal(&ctx->queue_cond); // 通知推流线程
    }
    encode_mutex_unlock(&ctx->queue_lock);

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
        // 从空闲池获取一个包结构
        AVPacket *queue_pkt = encoder_get_packet(ctx);
        if (!queue_pkt) {
            // 无法获取包，丢弃此帧（但数据已丢失，只能跳过）
            continue;
        }
        // 将 tmp_pkt 的数据所有权转移给 queue_pkt
        av_packet_move_ref(queue_pkt, out_st->tmp_pkt);
        // 调用 enqueue_avpacket 将包放入队列
        enqueue_avpacket(ctx, queue_pkt);

        // 注意：此时 tmp_pkt 已被清空，可继续用于下一次接收
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

int encoder_init(EncoderContext **pctx, int w, int h, int fps, int thread,
                 int internal_queue_size, enum AVCodecID codec_id) {
    if (fps == 0) {
        fps = STREAM_FRAME_RATE;
    }
    // 初始化EncoderContext
    EncoderContext *ctx = av_mallocz(sizeof(EncoderContext));
    if (!ctx)
        return -1;

    ctx->codec_id = codec_id; // 保存编码器类型
    OutputStream *out_st = &ctx->out_st;
    const AVCodec **codec_ptr = &ctx->codec;
    int ret;

    // 添加视频流
    printf("[设置编码器] ");
    ret = set_codec(out_st, codec_ptr, codec_id, w, h, fps, thread);
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
    // 初始化队列
    ctx->packet_queue = av_fifo_alloc(internal_queue_size * sizeof(AVPacket *));
    if (!ctx->packet_queue) {
        fprintf(stderr, "无法分配包队列\n");
        goto fail;
    }
    encode_mutex_init(&ctx->queue_lock);
    encode_cond_init(&ctx->queue_cond);
    ctx->encoding_finished = 0;

    // 初始化空闲包池（大小为数据队列大小的2倍，可根据实际调整）
    int pool_size = internal_queue_size * 2;
    ctx->free_packet_queue = av_fifo_alloc(pool_size * sizeof(AVPacket *));
    if (!ctx->free_packet_queue) {
        fprintf(stderr, "无法分配空闲包队列\n");
        goto fail;
    }
    encode_mutex_init(&ctx->pool_lock);

    // 预分配 AVPacket 并放入空闲池
    for (int i = 0; i < pool_size; i++) {
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            fprintf(stderr, "预分配包失败\n");
            goto fail;
        }
        av_fifo_generic_write(ctx->free_packet_queue, &pkt, sizeof(AVPacket *),
                              NULL);
    }

    // 初始化输出目标相关内容
    encode_mutex_init(&ctx->targets_lock);
    ctx->target = NULL;
    ctx->num_targets = 0;

    // 初始化统计
    ctx->stats.frame_count = 0;
    ctx->stats.packet_count = 0;
    ctx->stats.dropped_count = 0;
    ctx->stats.start_time = av_gettime();
    ctx->stats.last_time = ctx->stats.start_time;
    ctx->stats.last_frame_count = 0;
    encode_mutex_init(&ctx->stats_lock);

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
    if (ctx->packet_queue)
        av_fifo_freep(&ctx->packet_queue);
    if (ctx->free_packet_queue) {
        // 释放空闲池中的所有包
        AVPacket *pkt;
        while (av_fifo_size(ctx->free_packet_queue) > 0) {
            av_fifo_generic_read(ctx->free_packet_queue, &pkt,
                                 sizeof(AVPacket *), NULL);
            av_packet_free(&pkt);
        }
        av_fifo_freep(&ctx->free_packet_queue);
    }
    encode_mutex_destroy(&ctx->queue_lock);
    encode_cond_destroy(&ctx->queue_cond);
    encode_mutex_destroy(&ctx->pool_lock);
    encode_mutex_destroy(&ctx->targets_lock);
    encode_mutex_destroy(&ctx->stats_lock);
    av_free(ctx);
    return -1;
}
int encoder_add_output(EncoderContext *ctx, const char *filename) {
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    // 根据协议选择封装格式
    const char *format = NULL;
    if (strncmp(filename, "rtmp://", 7) == 0) {
        format = "flv";
    } else if (strncmp(filename, "rtsp://", 7) == 0) {
        format = "rtsp";
    } // 其他（如文件）保持 NULL，让 FFmpeg 自动猜测

    // 分配输出上下文（直接取地址）
    ret = avformat_alloc_output_context2(&fmt_ctx, NULL, format, filename);
    if (ret < 0 || !fmt_ctx) {
        fprintf(stderr, "创建输出上下文失败\n");
        return -1;
    }

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
    // 设置 RTSP 传输协议（TCP 或 UDP）
    AVDictionary *opts = NULL;
    if (strncmp(filename, "rtsp://", 7) == 0) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0); // 或 "udp"
    }

    // 写入文件头
    // printf("[写入输出文件头] ");
    ret = avformat_write_header(fmt_ctx, &opts);
    av_dict_free(&opts); // 释放字典

    if (ret < 0) {
        fprintf(stderr, "写入头失败: %s\n", av_err2str(ret));
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt_ctx->pb);
        }
        avformat_free_context(fmt_ctx);
        return -1;
    }

    // 将新输出目标放入动态数组
    encode_mutex_lock(&ctx->targets_lock);
    OutputTarget *new_target = av_realloc_array(
        ctx->target, ctx->num_targets + 1, sizeof(OutputTarget));
    if (!new_target) {
        encode_mutex_unlock(&ctx->targets_lock);
        // 关闭打开的资源
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
        target->base_set = 1;
        target->start_pts = 0; // 推流保持连续时间，不重置
    } else {
        target->base_set = 0;
        target->start_pts = 0; // 本地文件等待第一个包
    }

    encode_mutex_unlock(&ctx->targets_lock);
    printf("添加输出目标：%s\n", filename);
    return 0;
}
int encoder_remove_output(EncoderContext *ctx, const char *filename) {
    if (!ctx || !filename)
        return -1;

    encode_mutex_lock(&ctx->targets_lock);
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
            encode_mutex_unlock(&ctx->targets_lock);
            printf("移除输出目标：%s\n", filename);
            return 0;
        }
    }
    encode_mutex_unlock(&ctx->targets_lock);
    fprintf(stderr, "未找到输出目标: %s\n", filename);
    return -1;
}
int encoder_frame(EncoderContext *ctx, uint8_t *img_buf, int img_buf_size) {
    OutputStream *out_st = &(ctx)->out_st;
    int ret;

    if (ctx->codec_id == AV_CODEC_ID_MJPEG) {
        // MJPEG 直推模式：直接将 JPEG 数据打包为 AVPacket
        if (!img_buf) {
            fprintf(stderr, "MJPEG 直推必须提供图像数据\n");
            return -1;
        }
        // 从空闲池获取一个包结构
        AVPacket *pkt = encoder_get_packet(ctx);
        if (!pkt)
            return -1;

        // 为包分配数据缓冲区并拷贝 JPEG 数据
        if (av_new_packet(pkt, img_buf_size) < 0) {
            // 分配失败，归还包
            encode_mutex_lock(&ctx->pool_lock);
            if (av_fifo_space(ctx->free_packet_queue) >= sizeof(AVPacket *)) {
                av_fifo_generic_write(ctx->free_packet_queue, &pkt,
                                      sizeof(AVPacket *), NULL);
            } else {
                av_packet_free(&pkt);
            }
            encode_mutex_unlock(&ctx->pool_lock);
            return -1;
        }
        memcpy(pkt->data, img_buf, img_buf_size);
        pkt->size = img_buf_size;
        pkt->pts = out_st->next_pts++;
        pkt->dts = pkt->pts;           // MJPEG 无 B 帧
        pkt->flags |= AV_PKT_FLAG_KEY; // 每帧都是关键帧
        pkt->stream_index = 0;         // 暂未使用，队列中不依赖

        // 入队（内部处理队列满和丢包统计）
        ret = enqueue_avpacket(ctx, pkt);
        if (ret == 0) {
            encode_mutex_lock(&ctx->stats_lock);
            ctx->stats.frame_count++;
            encode_mutex_unlock(&ctx->stats_lock);
        }
        return ret; // 0 成功，-1 失败（包被丢弃）
    } else {
        // H.264 编码模式：原有逻辑
        if (img_buf == NULL) {
            // 使用内部生成模式
            ret = write_video_frame(ctx);
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
            ret = encode_and_write_frame(frame, ctx);
        }
        if (ret >= 0) { // 成功（0 或 1）
            encode_mutex_lock(&ctx->stats_lock);
            ctx->stats.frame_count++;
            encode_mutex_unlock(&ctx->stats_lock);
        }
    }
    return ret;
}
int encoder_output_packets(EncoderContext *ctx) {
    if (!ctx)
        return -1;
    AVPacket *pkt = NULL;

    // 从编码队列取出已编码的包
    encode_mutex_lock(&ctx->queue_lock);
    if (av_fifo_size(ctx->packet_queue) > 0) {
        av_fifo_generic_read(ctx->packet_queue, &pkt, sizeof(AVPacket *), NULL);
    }
    encode_mutex_unlock(&ctx->queue_lock);
    if (!pkt)
        return 1; // 队列未空

    // 分发到所有目标（需持有目标锁）
    encode_mutex_lock(&ctx->targets_lock);
    for (int i = 0; i < ctx->num_targets; i++) {
        OutputTarget *t = &ctx->target[i];
        AVPacket out_pkt; // 栈上分配
        av_packet_ref(&out_pkt, pkt);

        // 本地文件，获取第一个包时设置基准pts
        if (!t->base_set) {
            // t->start_pts = pkt->pts;
            // 如果使用b帧则用dts作为基准pts
            t->start_pts = pkt->dts;
            t->base_set = 1;
        }

        // 调整时间戳和流索引
        out_pkt.pts -= t->start_pts;
        out_pkt.dts -= t->start_pts;
        av_packet_rescale_ts(&out_pkt, ctx->out_st.enc_ctx->time_base,
                             t->st->time_base);
        out_pkt.stream_index = t->st->index;

        // 写入目标路径
        int ret = av_interleaved_write_frame(t->fmt_ctx, &out_pkt);
        if (ret < 0) {
            fprintf(stderr, "写入目标 %s 失败: %s\n", t->name, av_err2str(ret));
        }
        // else {
        //     // 对于文件目标，可以添加调试：确认写入的包大小
        //     if (strstr(t->name, ".mjpeg") || strstr(t->name, ".mp4")) {
        //         printf("Wrote packet of size %d to %s\n", out_pkt.size,
        //                t->name);
        //     }
        // }
        av_packet_unref(&out_pkt); // 释放 packet 结构
    }
    encode_mutex_unlock(&ctx->targets_lock);

    // 成功处理一个包后增加计数
    encode_mutex_lock(&ctx->stats_lock);
    ctx->stats.packet_count++;
    encode_mutex_unlock(&ctx->stats_lock);

    // 处理完 pkt 后，清空数据并归还空闲池
    av_packet_unref(pkt); // 释放数据引用
    encode_mutex_lock(&ctx->pool_lock);
    if (av_fifo_space(ctx->free_packet_queue) >= sizeof(AVPacket *)) {
        av_fifo_generic_write(ctx->free_packet_queue, &pkt, sizeof(AVPacket *),
                              NULL);
    } else {
        // 空闲池满（理论上不应发生），直接释放
        av_packet_free(&pkt);
    }
    encode_mutex_unlock(&ctx->pool_lock);

    return 0; // 成功
}

int encoder_queue_empty(EncoderContext *ctx) {
    if (!ctx)
        return 1; // 错误视为空
    return (av_fifo_size(ctx->packet_queue) == 0) ? 1 : 0;
}

void encoder_close(EncoderContext *ctx) {
    if (!ctx)
        return;
    OutputStream *out_st = &ctx->out_st; // 使用指针

    // 仅对 H.264 编码器刷新
    if (ctx->codec_id != AV_CODEC_ID_MJPEG && out_st->enc_ctx) {
        while (encode_and_write_frame(NULL, ctx) != 1)
            ;
    }

    // 设置编码结束标志
    encode_mutex_lock(&ctx->queue_lock);
    ctx->encoding_finished = 1;
    encode_cond_signal(&ctx->queue_cond);
    encode_mutex_unlock(&ctx->queue_lock);

    // 清空队列：输出所有剩余包
    int ret;
    do {
        ret = encoder_output_packets(ctx);
    } while (ret == 0); // 成功处理一个包就继续

    // 关闭所有输出目标
    encode_mutex_lock(&ctx->targets_lock);
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
    encode_mutex_unlock(&ctx->targets_lock);

    // 释放空闲池中的所有包
    encode_mutex_lock(&ctx->pool_lock);
    while (av_fifo_size(ctx->free_packet_queue) > 0) {
        AVPacket *pkt;
        av_fifo_generic_read(ctx->free_packet_queue, &pkt, sizeof(AVPacket *),
                             NULL);
        av_packet_free(&pkt);
    }
    av_fifo_freep(&ctx->free_packet_queue);
    encode_mutex_unlock(&ctx->pool_lock);

    // 销毁同步原语
    av_fifo_freep(&ctx->packet_queue);
    encode_mutex_destroy(&ctx->queue_lock);
    encode_cond_destroy(&ctx->queue_cond);
    encode_mutex_destroy(&ctx->pool_lock);
    encode_mutex_destroy(&ctx->targets_lock);
    encode_mutex_destroy(&ctx->stats_lock);

    // 释放编码器相关资源（原有）
    avcodec_free_context(&out_st->enc_ctx);
    // 释放编码器资源时，MJPEG 没有 frame，需注意空指针
    if (out_st->frame)
        av_frame_free(&out_st->frame);
    av_packet_free(&out_st->tmp_pkt);

    av_free(ctx->target);
    av_free(ctx);
    avformat_network_deinit();
}
void encoder_print_performance(EncoderContext *ctx) {
    if (!ctx)
        return;
    int64_t now = av_gettime();
    int64_t elapsed = now - ctx->stats.start_time; // 微秒
    double avg_fps = 0.0;
    double instant_fps = 0.0;
    int queue_size = 0;
    int free_pool_size = 0;
    int64_t frame_count, packet_count, dropped_count;

    encode_mutex_lock(&ctx->stats_lock);
    frame_count = ctx->stats.frame_count;
    packet_count = ctx->stats.packet_count;
    dropped_count = ctx->stats.dropped_count;

    if (elapsed > 0) {
        avg_fps = frame_count * 1000000.0 / elapsed;
    }
    int64_t since_last = now - ctx->stats.last_time;
    if (since_last > 0) {
        instant_fps = (frame_count - ctx->stats.last_frame_count) * 1000000.0 /
                      since_last;
    }
    // 更新 last 值
    ctx->stats.last_time = now;
    ctx->stats.last_frame_count = frame_count;
    encode_mutex_unlock(&ctx->stats_lock);

    // 获取队列长度
    encode_mutex_lock(&ctx->queue_lock);
    queue_size = av_fifo_size(ctx->packet_queue) / sizeof(AVPacket *);
    encode_mutex_unlock(&ctx->queue_lock);

    encode_mutex_lock(&ctx->pool_lock);
    free_pool_size = av_fifo_size(ctx->free_packet_queue) / sizeof(AVPacket *);
    encode_mutex_unlock(&ctx->pool_lock);

    printf("Performance: avg fps=%.2f, instant fps=%.2f, frames=%lld, "
           "packets=%lld, dropped=%lld, queue=%d, free_pool=%d\n",
           avg_fps, instant_fps, (long long)frame_count,
           (long long)packet_count, (long long)dropped_count, queue_size,
           free_pool_size);
}