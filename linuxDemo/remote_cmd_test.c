#include "remote_cmd.h"
#include <stdio.h>
#include <unistd.h>

// 参数更新计数器
static struct {
    int capture_enable;
    int crf;
    int max_bitrate;
    int gop;
} update_count = {0, 0, 0, 0};
void print_and_update(remote_cmd_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->param_lock);
    uint8_t mask = ctx->params.changed_mask;

    if (mask) {
        printf("=== 检测到参数变化 ===\n");

        if (remote_cmd_has_changed(ctx, CMD_ENABLE_CAPTURE)) {
            printf("  capture_enable: %d (更新次数: %d)\n",
                   ctx->params.capture_enable, ++update_count.capture_enable);
            // 清除标志
            remote_cmd_clear_changed(ctx, CMD_ENABLE_CAPTURE);
        }
        if (remote_cmd_has_changed(ctx, CMD_SET_CRF)) {
            printf("  crf: %d (更新次数: %d)\n", ctx->params.crf,
                   ++update_count.crf);
            remote_cmd_clear_changed(ctx, CMD_SET_CRF);
        }
        if (remote_cmd_has_changed(ctx, CMD_SET_MAX_BITRATE)) {
            printf("  max_bitrate: %d kbps (更新次数: %d)\n",
                   ctx->params.max_bitrate, ++update_count.max_bitrate);
            remote_cmd_clear_changed(ctx, CMD_SET_MAX_BITRATE);
        }
        if (remote_cmd_has_changed(ctx, CMD_SET_GOP)) {
            printf("  gop: %d (更新次数: %d)\n", ctx->params.gop,
                   ++update_count.gop);
            remote_cmd_clear_changed(ctx, CMD_SET_GOP);
        }
        printf("当前所有参数: cap=%d, crf=%d, bitrate=%d, gop=%d\n",
               ctx->params.capture_enable, ctx->params.crf,
               ctx->params.max_bitrate, ctx->params.gop);
    }
    pthread_mutex_unlock(&ctx->param_lock);
}

int main() {
    remote_cmd_ctx_t *remote_ctx;
    const char *server_url = "http://192.168.1.3:5000"; // 替换为你的服务器地址
    uint32_t device_id = 1;
    remote_cmd_init(server_url, device_id);
    if (!remote_ctx) {
        fprintf(stderr, "初始化失败\n");
        return -1;
    }
    printf("开始轮询，服务器: %s, 设备ID: %u\n", server_url, device_id);

    while (1) {
        // 1. 从服务器获取最新指令（内部加锁更新参数表）
        int ret = remote_cmd_fetch_and_update(remote_ctx);
        if (ret != 0) {
            // 网络错误，稍后重试
            printf("网络错误，等待2秒...\n");
            sleep(2);
            continue;
        }

        // 2. 检查是否有参数变化并打印
        print_and_update(remote_ctx);

        // 轮询间隔，避免过于频繁
        sleep(1);
    }

    remote_cmd_cleanup(remote_ctx);
    return 0;
}