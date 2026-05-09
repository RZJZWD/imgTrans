#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct {
    float cpu_usage; // 总体 CPU 使用率百分比 (0~100)
    float mem_usage; // 内存使用率百分比 (0~100)
} sys_stats_t;

int system_monitor_get(sys_stats_t *stats);

/**
 * @brief 获取 WiFi 信息（SSID 和 IP）
 * @param ssid_buf 输出 SSID 的缓冲区
 * @param ssid_len 缓冲区大小
 * @param ip_buf 输出 IP 的缓冲区
 * @param ip_len 缓冲区大小
 * @return 0 成功，-1 失败（至少 IP 可能获取到）
 */
int system_get_network_info(char *ssid_buf, size_t ssid_len, char *ip_buf,
                            size_t ip_len);

#ifdef __cplusplus
}
#endif
#endif