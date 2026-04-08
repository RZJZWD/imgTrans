#include "remote_cmd.h"
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
} write_buf_t;

// 内部函数

//  写回调，用于接收 HTTP 响应
static size_t write_callback(void *ptr, size_t size, size_t nmemb,
                             void *userdata) {
    write_buf_t *buf = (write_buf_t *)userdata;
    size_t total = size * nmemb;
    char *new_data = realloc(buf->data, buf->len + total + 1);
    if (!new_data)
        return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

// 发送 HTTP GET 请求，返回响应字符串（调用者提供内存并释放）
static int http_get(CURL *curl, const char *url, char **response) {
    write_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (res != CURLE_OK || http_code != 200) {
        free(buf.data);
        return -1;
    }
    *response = buf.data;
    return 0;
}
// 发送 HTTP POST 请求（JSON 数据），可选response返回响应（调用者释放）
static int http_post(CURL *curl, const char *url, const char *post_data,
                     char **response) {
    write_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || http_code != 200) {
        free(buf.data);
        return -1;
    }

    if (response) {
        *response = buf.data;
    } else {
        free(buf.data);
    }
    *response = buf.data;
    return 0;
}

/**
 * @brief 解析单条指令，更新参数表（调用者需保证 params 已加锁）
 * {
 *     "cmd": "enable_capture",
 *     "params": {
 *         "enable": true
 *     }
 * }
 * {
 *     "cmd": "set_crf",
 *     "params": {
 *         "crf": 23
 *     }
 * }
 * @param params 参数表指针
 * @param cmd_obj 包含 "cmd" 和 "params" 的 cJSON 对象
 */
static void parse_single_command(remote_cmd_params_t *params, cJSON *cmd_obj) {
    remote_cmd_ctx_t *ctx =
        remote_cmd_get_parent(params, remote_cmd_ctx_t, params);

    cJSON *cmd = cJSON_GetObjectItem(cmd_obj, "cmd");
    if (!cmd || !cJSON_IsString(cmd)) {
        return;
    }
    const char *cmd_str = cmd->valuestring;
    cJSON *params_obj = cJSON_GetObjectItem(cmd_obj, "params");

    if (strcmp(cmd_str, "enable_capture") == 0 && params_obj) {
        cJSON *enable = cJSON_GetObjectItem(params_obj, "enable");
        if (enable && cJSON_IsBool(enable)) {
            // 检查是否改变
            int new_val = enable->valueint ? 1 : 0;
            if (params->capture_enable != new_val) {
                params->capture_enable = new_val;
                remote_cmd_set_changed(ctx, CMD_ENABLE_CAPTURE);
            }
        }
    } else if (strcmp(cmd_str, "set_crf") == 0 && params_obj) {
        cJSON *crf_val = cJSON_GetObjectItem(params_obj, "crf");
        if (crf_val && cJSON_IsNumber(crf_val)) {
            int v = crf_val->valueint;
            if (v >= 0 && v <= 51 && params->crf != v) {
                params->crf = v;
                remote_cmd_set_changed(ctx, CMD_SET_CRF);
            }
        }
    } else if (strcmp(cmd_str, "set_max_bitrate") == 0 && params_obj) {
        cJSON *bitrate = cJSON_GetObjectItem(params_obj, "bitrate");
        if (bitrate && cJSON_IsNumber(bitrate)) {
            int v = bitrate->valueint;
            if (v > 0 && params->max_bitrate != v) {
                params->max_bitrate = v;
                remote_cmd_set_changed(ctx, CMD_SET_MAX_BITRATE);
            }
        }
    } else if (strcmp(cmd_str, "set_gop") == 0 && params_obj) {
        cJSON *gop_val = cJSON_GetObjectItem(params_obj, "gop");
        if (gop_val && cJSON_IsNumber(gop_val)) {
            int v = gop_val->valueint;
            if (v > 0 && params->gop != v) {
                params->gop = v;
                remote_cmd_set_changed(ctx, CMD_SET_GOP);
            }
        }
    }
}
/**
 * @brief 解析服务器返回的指令 JSON，更新参数表（调用者需加锁）
 *
 * 服务器返回的 JSON 格式示例：
 * {
 *     "commands": [
 *         {"cmd": "set_crf", "params": {"crf": 23}},
 *         {"cmd": "set_gop", "params": {"gop": 50}}
 *     ]
 * }
 *
 * @param params 参数表指针（调用者需保证 params 已加锁）
 * @param json_str 接收到的 JSON 字符串
 */
static void parse_json_to_params(remote_cmd_params_t *params,
                                 const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root)
        return;

    cJSON *ccommands = cJSON_GetObjectItem(root, "commands");
    if (ccommands && cJSON_IsArray(ccommands)) {
        int size = cJSON_GetArraySize(ccommands);
        for (int i = 0; i < size; i++) {
            cJSON *cmd_obj = cJSON_GetArrayItem(ccommands, i);
            parse_single_command(params, cmd_obj);
        }
    } else {
        parse_single_command(params, root);
    }
    cJSON_Delete(root);
}

// 外部接口
/**
 * @brief 初始化远程指令模块
 * @param api_endpoint 服务器API基础地址，例如 "http://192.168.1.100:5000"
 * @param device_id 设备ID
 * @return 0成功，-1失败
 */
remote_cmd_ctx_t *remote_cmd_init(const char *api_endpoint,
                                  uint32_t device_id) {

    if (!api_endpoint)
        return NULL;
    curl_global_init(CURL_GLOBAL_ALL);

    remote_cmd_ctx_t *ctx =
        (remote_cmd_ctx_t *)malloc(sizeof(remote_cmd_ctx_t));

    ctx->curl_handle = curl_easy_init();
    if (!ctx->curl_handle) {
        curl_global_cleanup();
        free(ctx);
        return NULL;
    }

    strncpy(ctx->api_endpoint, api_endpoint, sizeof(ctx->api_endpoint) - 1);
    ctx->api_endpoint[sizeof(ctx->api_endpoint) - 1] = '\0';
    ctx->device_id = device_id;

    pthread_mutex_init(&ctx->param_lock, NULL);
    // 初始化参数表：所有标志位为0，各值默认为0（含义由标志位决定）
    ctx->params.changed_mask = 0;
    ctx->params.capture_enable = 0;
    ctx->params.crf = 0;
    ctx->params.max_bitrate = 0;
    ctx->params.gop = 0;
    return ctx;
}
/**
 * @brief 从服务器获取指令并更新内部参数表（远程线程调用）
 * @param ctx 远程模块上下文
 * @return 0成功，-1网络错误或解析失败
 */
int remote_cmd_fetch_and_update(remote_cmd_ctx_t *ctx) {
    if (!ctx || !ctx->curl_handle)
        return -1;

    char url[512];
    snprintf(url, sizeof(url), "%s/command?device_id=%u", ctx->api_endpoint,
             ctx->device_id);

    char *response = NULL;
    if (http_get(ctx->curl_handle, url, &response) != 0) {
        return -1; // 网络错误
    }

    // 加锁更新参数表
    pthread_mutex_lock(&ctx->param_lock);
    parse_json_to_params(&ctx->params, response);
    pthread_mutex_unlock(&ctx->param_lock);

    free(response);
    return 0;
}
/**
 * @brief 从远程质量模块上下文获取更新后的参数
 * @note 调用者需保证 params 已加锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 * @return int
 */
int remote_cmd_get_param(remote_cmd_ctx_t *ctx, remote_cmd_type_t type) {
    if (!ctx)
        return -1;
    int value = 0;

    switch (type) {
    case CMD_ENABLE_CAPTURE:
        value = ctx->params.capture_enable;
        break;
    case CMD_SET_CRF:
        value = ctx->params.crf;
        break;
    case CMD_SET_MAX_BITRATE:
        value = ctx->params.max_bitrate;
        break;
    case CMD_SET_GOP:
        value = ctx->params.gop;
        break;
    default:
        value = -1;
        break;
    }

    return value;
}
/**
 * @brief 清理资源
 * @param ctx 远程模块上下文
 */
void remote_cmd_cleanup(remote_cmd_ctx_t *ctx) {
    if (!ctx)
        return;

    if (ctx->curl_handle) {
        curl_easy_cleanup(ctx->curl_handle);
        curl_global_cleanup();
        ctx->curl_handle = NULL;
    }

    pthread_mutex_destroy(&ctx->param_lock);

    // 释放结构体
    free(ctx);
}
/**
 * @brief 设置指定指令的变化标志（表示有新请求）
 * @note 调用者需保证 params 已加锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 */
void remote_cmd_set_changed(remote_cmd_ctx_t *ctx, remote_cmd_type_t type) {
    if (!ctx)
        return;
    ctx->params.changed_mask |= (1 << (type - 1));
}
/**
 * @brief 检查指定指令是否有变化
 * @note 调用者需保证 params 已加锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 * @return 1表示有变化，0表示无变化
 */
int remote_cmd_has_changed(remote_cmd_ctx_t *ctx, remote_cmd_type_t type) {
    if (!ctx)
        return 0;
    return (ctx->params.changed_mask & (1 << (type - 1))) != 0;
}
/**
 * @brief 清除指定指令的变化标志（应用后调用）
 * @note 调用者需保证 params 已加锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 */
void remote_cmd_clear_changed(remote_cmd_ctx_t *ctx, remote_cmd_type_t type) {
    if (!ctx)
        return;
    ctx->params.changed_mask &= ~(1 << (type - 1));
}