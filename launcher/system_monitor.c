#include "system_monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long long prev_user = 0, prev_nice = 0, prev_system = 0,
                          prev_idle = 0, prev_iowait = 0, prev_irq = 0,
                          prev_softirq = 0, prev_steal = 0;
static int first_run = 1;

int system_monitor_get(sys_stats_t *stats) {
    if (!stats)
        return -1;

    // --- CPU 使用率 ---
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp)
        return -1;

    char line[256];
    fgets(line, sizeof(line), fp);
    fclose(fp);

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice,
           &system, &idle, &iowait, &irq, &softirq, &steal);

    if (first_run) {
        first_run = 0;
        stats->cpu_usage = 0.0f;
    } else {
        unsigned long long total_prev = prev_user + prev_nice + prev_system +
                                        prev_idle + prev_iowait + prev_irq +
                                        prev_softirq + prev_steal;
        unsigned long long total_cur =
            user + nice + system + idle + iowait + irq + softirq + steal;
        unsigned long long total_diff = total_cur - total_prev;
        unsigned long long idle_diff = idle - prev_idle;

        if (total_diff > 0)
            stats->cpu_usage =
                (float)(total_diff - idle_diff) / total_diff * 100.0f;
        else
            stats->cpu_usage = 0.0f;
    }
    prev_user = user;
    prev_nice = nice;
    prev_system = system;
    prev_idle = idle;
    prev_iowait = iowait;
    prev_irq = irq;
    prev_softirq = softirq;
    prev_steal = steal;

    // --- 内存使用率 ---
    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return -1;

    unsigned long long mem_total = 0, mem_available = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0)
            sscanf(line, "MemTotal: %llu", &mem_total);
        else if (strncmp(line, "MemAvailable:", 13) == 0)
            sscanf(line, "MemAvailable: %llu", &mem_available);
    }
    fclose(fp);

    if (mem_total > 0)
        stats->mem_usage =
            (float)(mem_total - mem_available) / mem_total * 100.0f;
    else
        stats->mem_usage = 0.0f;

    return 0;
}

// ... 原有 CPU/内存统计代码 ...

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- 网络信息获取 ---------- */
static int get_wifi_ssid(char *buf, size_t len) {
    FILE *fp = popen(
        "wpa_cli -i wlan0 status 2>/dev/null | grep '^ssid=' | cut -d'=' -f2",
        "r");
    if (!fp)
        return -1;
    if (fgets(buf, len, fp) == NULL) {
        pclose(fp);
        return -1;
    }
    // 移除尾部换行
    size_t slen = strlen(buf);
    if (slen > 0 && buf[slen - 1] == '\n')
        buf[slen - 1] = '\0';
    pclose(fp);
    return 0;
}

int system_get_network_info(char *ssid_buf, size_t ssid_len, char *ip_buf,
                            size_t ip_len) {
    int got_ssid = 0, got_ip = 0;

    // 获取 IP（原 system_get_ip 逻辑内联）
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL)
                continue;
            if (ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;
            if (!(ifa->ifa_flags & IFF_UP))
                continue;

            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            const char *ip = inet_ntoa(sin->sin_addr);
            if (ip && strcmp(ip, "127.0.0.1") != 0) {
                snprintf(ip_buf, ip_len, "%s", ip);
                got_ip = 1;
                break;
            }
        }
        freeifaddrs(ifaddr);
    }

    // 获取 SSID
    if (get_wifi_ssid(ssid_buf, ssid_len) == 0) {
        got_ssid = 1;
    } else {
        snprintf(ssid_buf, ssid_len, "Unknown");
    }

    if (got_ip || got_ssid)
        return 0;
    return -1;
}