#ifndef _REMOTE_CMD_H
#define _REMOTE_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 指令枚举类型
// 其中枚举的指令类型不可以跳过，因为参数表依靠枚举量作为位域计算的基础
typedef enum {
    CMD_NONE = 0,
    CMD_ENABLE_CAPTURE,
    CMD_SET_CRF,
    CMD_SET_MAX_BITRATE,
    CMD_SET_GOP,
    CMD_NUMS,
} remote_cmd_type_t;

// 远程指令模块内部参数结构体
typedef struct {
    uint8_t changed_mask; // 变化标志位，0 表示无新请求
    int capture_enable;   // 0=停止, 1=开始
    int crf;              // 0~51
    int max_bitrate;      // 码率(kbps)
    int gop;              // GOP长度
} remote_cmd_params_t;

// 设备上下文结构体
typedef struct {
    CURL *curl_handle;     // curl句柄
    char api_endpoint[64]; // 控制端API地址
    uint32_t device_id;    // 设备唯一标识

    remote_cmd_params_t params; // 参数表
    pthread_mutex_t param_lock; // 参数表访问锁
} remote_cmd_ctx_t;

/**
 * @brief 初始化远程指令模块
 * @param ctx 远程模块上下文
 * @param api_endpoint 服务器API基础地址，例如 "http://192.168.1.100:5000"
 * @param device_id 设备ID
 * @return 0成功，-1失败
 */
int remote_cmd_init(remote_cmd_ctx_t *ctx, const char *api_endpoint,
                    uint32_t device_id);

/**
 * @brief 从服务器获取指令并更新内部参数表（远程线程调用）
 * @param ctx 远程模块上下文
 * @return 0成功，-1网络错误或解析失败
 */
int remote_cmd_fetch_and_update(remote_cmd_ctx_t *ctx);

/**
 * @brief 从远程质量模块上下文获取更新后的参数
 * @param ctx 远程模块上下文
 * @param type 指令类型
 * @return int
 */
int remote_cmd_get_param(remote_cmd_ctx_t *ctx, remote_cmd_type_t type);

/**
 * @brief 清理资源
 * @param ctx 远程模块上下文
 */
void remote_cmd_cleanup(remote_cmd_ctx_t *ctx);

/**
 * @brief 设置指定指令的变化标志（表示有新请求）
 * @note 调用此函数前必须持有 ctx->param_lock 锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 */
void remote_cmd_set_changed(remote_cmd_ctx_t *ctx, remote_cmd_type_t type);

/**
 * @brief 检查指定指令是否有新请求
 * @note 调用此函数前必须持有 ctx->param_lock 锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 * @return 1表示有变化，0表示无变化
 */
int remote_cmd_has_changed(remote_cmd_ctx_t *ctx, remote_cmd_type_t type);

/**
 * @brief 清除指定指令的变化标志（应用后调用）
 * @note 调用此函数前必须持有 ctx->param_lock 锁
 * @param ctx 远程模块上下文
 * @param type 指令类型
 */
void remote_cmd_clear_changed(remote_cmd_ctx_t *ctx, remote_cmd_type_t type);

// 通过结构体成员获取父结构体
#define remote_cmd_get_parent(ptr, type, member)                               \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#ifdef __cplusplus
}
#endif
#endif //_REMOTE_CMD_H