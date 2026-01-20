#ifndef _DISPLAY_RGB_H
#define _DISPLAY_RGB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 显示配置结构体
 */
typedef struct {
    int show_info_label;   // 是否显示信息标签
    int show_exit_label;   // 是否显示退出提示标签
    int info_label_x;      // 信息标签X位置偏移
    int info_label_y;      // 信息标签Y位置偏移
    const char *info_text; // 自定义信息文本（NULL则使用默认）
} DisplayLabelConfig;

/**
 * @brief 初始化RGB显示系统
 * @param config 显示配置（可以为NULL，使用默认配置）
 * @return 成功返回0，失败返回-1
 */
int display_rgb_init(DisplayLabelConfig *config);

/**
 * @brief 从文件加载并显示RGB图像
 * @param filename RGB文件路径
 * @return 成功返回0，失败返回-1
 */
int display_rgb_from_file(const char *filename);

/**
 * @brief 从内存缓冲区显示RGB图像
 * @param data RGB888数据缓冲区
 * @param width 图像宽度
 * @param height 图像高度
 * @return 成功返回0，失败返回-1
 */
int display_rgb_from_buffer(uint8_t *data, int width, int height);

/**
 * @brief 创建并显示测试图像
 * @param width 图像宽度
 * @param height 图像高度
 * @return 成功返回0，失败返回-1
 */
int display_rgb_test_image(int width, int height);

/**
 * @brief 更新信息标签文本
 * @param text 新的文本内容（支持格式化字符串）
 * @param ... 格式化参数
 * @return 成功返回0，失败返回-1
 */
int display_rgb_update_label(const char *text, ...);

/**
 * @brief 运行显示主循环
 * @note 调用此函数后，程序将进入显示循环
 */
void display_rgb_run(void);

/**
 * @brief 停止显示循环
 */
void display_rgb_stop(void);

/**
 * @brief 获取当前显示状态
 * @return 1表示正在运行，0表示已停止
 */
int display_rgb_is_running(void);

/**
 * @brief 清理显示资源
 */
void display_rgb_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_RGB_H