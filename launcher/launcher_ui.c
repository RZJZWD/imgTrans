#include "launcher_ui.h"
#include "system_monitor.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONFIG_FILE "/tmp/img_trans_launcher.conf"

/* ---------- 全局 ---------- */
static const char *g_img_trans_path = NULL;

/* ---------- UI 控件 ---------- */
static lv_obj_t *scr;
static lv_obj_t *cont_config, *cont_monitor;
static lv_obj_t *label_location;

// 配置控件
static lv_obj_t *dd_resolution;
static lv_obj_t *ta_fps;    // 改为文本框
static lv_obj_t *dd_format; // 采集格式（jpeg / yuyv）
static lv_obj_t *dd_encode; // 编码格式（h264 / mjpeg）
static lv_obj_t *sw_display;

// 推流相关
static lv_obj_t *dd_protocol;
static lv_obj_t *label_port;
static lv_obj_t *ta_ip;
static lv_obj_t *ta_stream;
static lv_obj_t *ta_device_id;

// 网络状态（配置界面）
static lv_obj_t *label_network;
static lv_timer_t *net_timer = NULL;

// 监控界面控件
static lv_obj_t *chart_cpu, *chart_mem;
static lv_obj_t *label_cpu_val, *label_mem_val;
static lv_chart_series_t *ser_cpu, *ser_mem;
static lv_timer_t *mon_timer;
static lv_obj_t *label_stream_url;

static pid_t img_pid = 0;

// 键盘
static lv_obj_t *kb = NULL;

/* ---------- 内部函数声明 ---------- */
static void load_config(void);
static void save_config(void);
static void build_img_cmd(char *cmd, size_t size);
static void start_img_trans(void);
static void stop_img_trans(void);
static void switch_to_monitor(void);
static void switch_to_config(void);
static void mon_timer_cb(lv_timer_t *t);
static void net_timer_cb(lv_timer_t *t);
static void btn_save_cb(lv_event_t *e);
static void btn_start_cb(lv_event_t *e);
static void btn_stop_cb(lv_event_t *e);
static void protocol_changed_cb(lv_event_t *e);
static void textarea_clicked_cb(lv_event_t *e);
static void kb_ok_cb(lv_event_t *e);
static void kb_close_cb(lv_event_t *e);
static void update_stream_url_label(void);
static int get_clamped_fps(void); // 获取并修正帧率

/* ---------- 公开接口 ---------- */
void launcher_ui_create(const char *img_trans_path, const char *location) {
    g_img_trans_path = img_trans_path;

    scr = lv_screen_active();

    // 位置标签
    label_location = lv_label_create(scr);
    lv_label_set_text_fmt(label_location, "Location: %s", location);
    lv_obj_align(label_location, LV_ALIGN_TOP_MID, 0, 5);

    /* ---------- 配置容器 ---------- */
    cont_config = lv_obj_create(scr);
    lv_obj_set_size(cont_config, 940, 540);
    lv_obj_align(cont_config, LV_ALIGN_CENTER, 0, 10);

    // 网络信息标签 (左上角)
    label_network = lv_label_create(cont_config);
    lv_label_set_text(label_network, "WiFi: checking...");
    lv_obj_align(label_network, LV_ALIGN_TOP_LEFT, 10, 10);

    int left_x = 10;
    int y = 40; // 下移让出网络标签位置

    // ---- 分辨率预设 ----
    lv_obj_t *lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Resolution:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, left_x, y);
    dd_resolution = lv_dropdown_create(cont_config);
    lv_dropdown_set_options(dd_resolution, "480p\n720p");
    lv_dropdown_set_selected(dd_resolution, 0);
    lv_obj_set_size(dd_resolution, 120, 35);
    lv_obj_align_to(dd_resolution, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    y += 65;

    // ---- 帧率 (改为文本框 + 数字键盘) ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Frame Rate:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, left_x, y);
    ta_fps = lv_textarea_create(cont_config);
    lv_textarea_set_placeholder_text(ta_fps, "1-60");
    lv_obj_set_size(ta_fps, 80, 30);
    lv_obj_align_to(ta_fps, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_textarea_set_one_line(ta_fps, true);
    lv_textarea_set_text(ta_fps, "25"); // 默认25帧
    lv_obj_add_event_cb(ta_fps, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);
    y += 65;

    // ---- 采集格式 ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Capture:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, left_x, y);
    dd_format = lv_dropdown_create(cont_config);
    lv_dropdown_set_options(dd_format, "jpeg\nyuyv");
    lv_dropdown_set_selected(dd_format, 1); // 默认 yuyv
    lv_obj_set_size(dd_format, 100, 35);
    lv_obj_align_to(dd_format, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    y += 65;

    // ---- 编码格式 (新增) ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Encode:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, left_x, y);
    dd_encode = lv_dropdown_create(cont_config);
    lv_dropdown_set_options(dd_encode, "h264\nmjpeg");
    lv_dropdown_set_selected(dd_encode, 0); // 默认 h264
    lv_obj_set_size(dd_encode, 100, 35);
    lv_obj_align_to(dd_encode, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    y += 65;

    // ---- 本地显示 ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Local Display:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, left_x, y);
    sw_display = lv_switch_create(cont_config);
    lv_obj_remove_state(sw_display, LV_STATE_CHECKED); // 默认关闭
    lv_obj_align_to(sw_display, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    y += 55;

    // 右列控件
    int right_x = 490;
    y = 40; // 与左列等高

    // ---- 协议选择 ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Protocol:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, right_x, y);
    dd_protocol = lv_dropdown_create(cont_config);
    lv_dropdown_set_options(dd_protocol, "RTSP\nRTMP");
    lv_obj_set_size(dd_protocol, 100, 35);
    lv_obj_align_to(dd_protocol, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_obj_add_event_cb(dd_protocol, protocol_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    label_port = lv_label_create(cont_config);
    lv_label_set_text(label_port, "Port: 8554");
    lv_obj_align_to(label_port, dd_protocol, LV_ALIGN_OUT_RIGHT_MID, 15, 0);
    y += 65;

    // ---- Server IP ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Server IP:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, right_x, y);
    ta_ip = lv_textarea_create(cont_config);
    lv_textarea_set_placeholder_text(ta_ip, "192.168.1.3");
    lv_obj_set_size(ta_ip, 200, 30);
    lv_obj_align_to(ta_ip, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_textarea_set_one_line(ta_ip, true);
    lv_obj_add_event_cb(ta_ip, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);
    y += 65;

    // ---- Stream Name ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Stream:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, right_x, y);
    ta_stream = lv_textarea_create(cont_config);
    lv_textarea_set_placeholder_text(ta_stream, "live/stream");
    lv_obj_set_size(ta_stream, 200, 30);
    lv_obj_align_to(ta_stream, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_textarea_set_one_line(ta_stream, true);
    lv_obj_add_event_cb(ta_stream, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);
    y += 65;

    // ---- Device ID ----
    lbl = lv_label_create(cont_config);
    lv_label_set_text(lbl, "Device ID:");
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, right_x, y);
    ta_device_id = lv_textarea_create(cont_config);
    lv_textarea_set_placeholder_text(ta_device_id, "1");
    lv_obj_set_size(ta_device_id, 80, 30);
    lv_obj_align_to(ta_device_id, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_textarea_set_one_line(ta_device_id, true);
    lv_obj_add_event_cb(ta_device_id, textarea_clicked_cb, LV_EVENT_CLICKED,
                        NULL);
    y += 65;

    // ---- 按钮 ----
    lv_obj_t *btn_save = lv_btn_create(cont_config);
    lv_obj_set_size(btn_save, 120, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_LEFT, 200, -20);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save");
    lv_obj_center(lbl_save);
    lv_obj_add_event_cb(btn_save, btn_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_start = lv_btn_create(cont_config);
    lv_obj_set_size(btn_start, 120, 40);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_RIGHT, -200, -20);
    lv_obj_t *lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, "Start");
    lv_obj_center(lbl_start);
    lv_obj_add_event_cb(btn_start, btn_start_cb, LV_EVENT_CLICKED, NULL);

    /* ---------- 监控容器 ---------- */
    cont_monitor = lv_obj_create(scr);
    lv_obj_set_size(cont_monitor, 940, 540);
    lv_obj_align(cont_monitor, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(cont_monitor, LV_OBJ_FLAG_HIDDEN);

    // 推流URL标签
    label_stream_url = lv_label_create(cont_monitor);
    lv_label_set_text(label_stream_url, "Stream: --");
    lv_obj_align(label_stream_url, LV_ALIGN_TOP_LEFT, 10, 10);

    chart_cpu = lv_chart_create(cont_monitor);
    lv_obj_set_size(chart_cpu, 880, 200);
    lv_obj_align(chart_cpu, LV_ALIGN_TOP_MID, 0, 40);
    lv_chart_set_type(chart_cpu, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_cpu, 60);
    lv_chart_set_range(chart_cpu, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    ser_cpu = lv_chart_add_series(chart_cpu, lv_color_hex(0xFF0000),
                                  LV_CHART_AXIS_PRIMARY_Y);
    label_cpu_val = lv_label_create(cont_monitor);
    lv_label_set_text(label_cpu_val, "CPU: 0%");
    lv_obj_align_to(label_cpu_val, chart_cpu, LV_ALIGN_OUT_TOP_LEFT, 0, -5);

    chart_mem = lv_chart_create(cont_monitor);
    lv_obj_set_size(chart_mem, 880, 200);
    lv_obj_align(chart_mem, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_chart_set_type(chart_mem, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_mem, 60);
    lv_chart_set_range(chart_mem, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    ser_mem = lv_chart_add_series(chart_mem, lv_color_hex(0x00FF00),
                                  LV_CHART_AXIS_PRIMARY_Y);
    label_mem_val = lv_label_create(cont_monitor);
    lv_label_set_text(label_mem_val, "Mem: 0%");
    lv_obj_align_to(label_mem_val, chart_mem, LV_ALIGN_OUT_TOP_LEFT, 0, -5);

    lv_obj_t *btn_stop = lv_btn_create(cont_monitor);
    lv_obj_set_size(btn_stop, 120, 40);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_t *lbl_stop = lv_label_create(btn_stop);
    lv_label_set_text(lbl_stop, "Stop");
    lv_obj_center(lbl_stop);
    lv_obj_add_event_cb(btn_stop, btn_stop_cb, LV_EVENT_CLICKED, NULL);

    load_config();
    protocol_changed_cb(NULL);

    // 启动网络状态定时器
    net_timer = lv_timer_create(net_timer_cb, 1000, NULL);
}

/* ---------- 辅助函数：获取并修正帧率 ---------- */
static int get_clamped_fps(void) {
    const char *text = lv_textarea_get_text(ta_fps);
    int fps = atoi(text);
    if (fps < 1)
        fps = 1;
    if (fps > 60)
        fps = 60;
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", fps);
    lv_textarea_set_text(ta_fps, buf); // 去除前导0，保证显示无格式问题
    return fps;
}

/* ---------- 网络定时器回调 ---------- */
static void net_timer_cb(lv_timer_t *t) {
    char ssid[64] = "Unknown";
    char ip[32] = "Unknown";
    system_get_network_info(ssid, sizeof(ssid), ip, sizeof(ip));
    lv_label_set_text_fmt(label_network, "WiFi: %s / %s", ssid, ip);
}

/* ---------- 协议切换回调 ---------- */
static void protocol_changed_cb(lv_event_t *e) {
    LV_UNUSED(e);
    uint16_t sel = lv_dropdown_get_selected(dd_protocol);
    lv_label_set_text(label_port, (sel == 0) ? "Port: 8554" : "Port: 1935");
}

/* ---------- 文本区域点击弹出键盘 ---------- */
static void textarea_clicked_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    if (kb)
        lv_obj_del(kb);
    kb = lv_keyboard_create(lv_screen_active());

    // 对于IP、设备ID、帧率使用数字键盘
    if (ta == ta_ip || ta == ta_device_id || ta == ta_fps) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    } else {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }

    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kb_ok_cb, LV_EVENT_READY, ta);
    lv_obj_add_event_cb(kb, kb_close_cb, LV_EVENT_CANCEL, NULL);
}

static void kb_ok_cb(lv_event_t *e) {
    lv_obj_del(kb);
    kb = NULL;
}

static void kb_close_cb(lv_event_t *e) {
    lv_obj_del(kb);
    kb = NULL;
}

/* ---------- 更新推流 URL 标签 ---------- */
static void update_stream_url_label(void) {
    const char *ip = lv_textarea_get_text(ta_ip);
    const char *stream = lv_textarea_get_text(ta_stream);
    int protocol = lv_dropdown_get_selected(dd_protocol);
    char encode_buf[16] = {0};
    lv_dropdown_get_selected_str(dd_encode, encode_buf, sizeof(encode_buf));
    int port = (protocol == 0) ? 8554 : 1935;
    const char *proto_str = (protocol == 0) ? "rtsp" : "rtmp";
    lv_label_set_text_fmt(label_stream_url, "Stream: %s://%s:%d/%s?codec=%s",
                          proto_str, ip, port, stream, encode_buf);
}

/* ---------- 界面切换 ---------- */
static void switch_to_monitor(void) {
    update_stream_url_label();
    lv_obj_add_flag(cont_config, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(cont_monitor, LV_OBJ_FLAG_HIDDEN);
    mon_timer = lv_timer_create(mon_timer_cb, 1000, NULL);
}

static void switch_to_config(void) {
    if (mon_timer) {
        lv_timer_del(mon_timer);
        mon_timer = NULL;
    }
    lv_obj_add_flag(cont_monitor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(cont_config, LV_OBJ_FLAG_HIDDEN);
}

static void mon_timer_cb(lv_timer_t *t) {
    sys_stats_t stats;
    if (system_monitor_get(&stats) == 0) {
        lv_chart_set_next_value(chart_cpu, ser_cpu, (int)stats.cpu_usage);
        lv_chart_set_next_value(chart_mem, ser_mem, (int)stats.mem_usage);
        lv_label_set_text_fmt(label_cpu_val, "CPU: %.1f%%", stats.cpu_usage);
        lv_label_set_text_fmt(label_mem_val, "Mem: %.1f%%", stats.mem_usage);
    }
}

/* ---------- 配置读写 ---------- */
static void load_config(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f)
        return;

    int res_idx, fps, cap_idx, encode_idx, show, protocol;
    char ip[32], stream[64], dev_id[8];
    if (fscanf(f, "%d %d %d %d %d %d %s %s %s", &res_idx, &fps, &cap_idx,
               &encode_idx, &show, &protocol, ip, stream, dev_id) == 9) {
        lv_dropdown_set_selected(dd_resolution, res_idx);
        char fps_str[8];
        snprintf(fps_str, sizeof(fps_str), "%d", fps);
        lv_textarea_set_text(ta_fps, fps_str);
        get_clamped_fps(); // 确保范围正确并去除前导0
        lv_dropdown_set_selected(dd_format, cap_idx);
        lv_dropdown_set_selected(dd_encode, encode_idx);
        if (show)
            lv_obj_add_state(sw_display, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_display, LV_STATE_CHECKED);
        lv_dropdown_set_selected(dd_protocol, protocol);
        lv_textarea_set_text(ta_ip, ip);
        lv_textarea_set_text(ta_stream, stream);
        lv_textarea_set_text(ta_device_id, dev_id);
    }
    fclose(f);
}

static void save_config(void) {
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f)
        return;

    int res_idx = lv_dropdown_get_selected(dd_resolution);
    int fps = get_clamped_fps(); // 获取修正后的帧率
    int cap_idx = lv_dropdown_get_selected(dd_format);
    int encode_idx = lv_dropdown_get_selected(dd_encode);
    int show = lv_obj_has_state(sw_display, LV_STATE_CHECKED) ? 1 : 0;
    int protocol = lv_dropdown_get_selected(dd_protocol);

    fprintf(f, "%d %d %d %d %d %d %s %s %s\n", res_idx, fps, cap_idx,
            encode_idx, show, protocol, lv_textarea_get_text(ta_ip),
            lv_textarea_get_text(ta_stream),
            lv_textarea_get_text(ta_device_id));
    fclose(f);
}

/* ---------- 构造 img_trans 命令 ---------- */
static void build_img_cmd(char *cmd, size_t size) {
    int res_idx = lv_dropdown_get_selected(dd_resolution);
    int w = (res_idx == 0) ? 640 : 1280;
    int h = (res_idx == 0) ? 480 : 720;

    int fps = get_clamped_fps(); // 使用修正后的帧率
    char cap_buf[16] = {0};
    lv_dropdown_get_selected_str(dd_format, cap_buf, sizeof(cap_buf));

    char encode_buf[16] = {0};
    lv_dropdown_get_selected_str(dd_encode, encode_buf, sizeof(encode_buf));

    int show = lv_obj_has_state(sw_display, LV_STATE_CHECKED) ? 1 : 0;

    const char *ip = lv_textarea_get_text(ta_ip);
    const char *stream = lv_textarea_get_text(ta_stream);
    const char *device_id = lv_textarea_get_text(ta_device_id);

    int protocol = lv_dropdown_get_selected(dd_protocol);
    int port = (protocol == 0) ? 8554 : 1935;
    const char *proto = (protocol == 0) ? "rtsp" : "rtmp";

    snprintf(cmd, size,
             "%s -c %d %d %d %s -s %d -o 1 %s://%s:%d/%s?codec=%s -r "
             "http://%s:5000?device_id=%s",
             g_img_trans_path, w, h, fps, cap_buf, show, proto, ip, port,
             stream, encode_buf, ip, device_id);
}

/* ---------- 进程管理 ---------- */
static void start_img_trans(void) {
    int show = lv_obj_has_state(sw_display, LV_STATE_CHECKED) ? 1 : 0;
    if (show) {
        char cmd[512];
        build_img_cmd(cmd, sizeof(cmd));
        execlp("/bin/sh", "/bin/sh", "-c", cmd, NULL);
        perror("exec failed");
        exit(1);
    } else {
        img_pid = fork();
        if (img_pid == 0) {
            char cmd[512];
            build_img_cmd(cmd, sizeof(cmd));
            execlp("/bin/sh", "/bin/sh", "-c", cmd, NULL);
            exit(1);
        } else if (img_pid > 0) {
            switch_to_monitor();
        }
    }
}

static void stop_img_trans(void) {
    if (img_pid > 0) {
        kill(img_pid, SIGTERM);
        waitpid(img_pid, NULL, 0);
        img_pid = 0;
    }
}

/* ---------- 按钮回调 ---------- */
static void btn_save_cb(lv_event_t *e) { save_config(); }
static void btn_start_cb(lv_event_t *e) {
    save_config();
    start_img_trans();
}
static void btn_stop_cb(lv_event_t *e) {
    stop_img_trans();
    switch_to_config();
}