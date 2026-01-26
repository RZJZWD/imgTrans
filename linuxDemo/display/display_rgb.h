#ifndef _DISPLAY_RGB_H
#define _DISPLAY_RGB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 视频录制状态结构
 */
typedef struct {
    int is_recording;          // 录制状态：0=未录制，1=录制中
    uint32_t file_counter;     // 文件计数器
    char current_filename[64]; // 当前文件名
    int width;                 // 视频宽度
    int height;                // 视频高度
    float fps;                 // 帧率
} VideoState;

/**
 * @brief UI回调函数类型
 */
typedef void (*PhotoCallback)(void);                       // 截屏回调
typedef void (*RecordStartCallback)(const char *filename); // 开始录制回调
typedef void (*RecordStopCallback)(void);                  // 停止录制回调
/**
 * @brief 设置UI事件回调函数
 * @param photo_cb 拍照按钮回调
 * @param record_start_cb 开始录制回调
 * @param record_stop_cb 停止录制回调
 */
void display_rgb_set_callbacks(PhotoCallback photo_cb,
                               RecordStartCallback record_start_cb,
                               RecordStopCallback record_stop_cb);

/**
 * @brief 更新视频信息显示
 * @param width 视频宽度
 * @param height 视频高度
 * @param fps 帧率
 */
void display_rgb_update_video_info(int width, int height, float fps);

/**
 * @brief 初始化RGB显示系统
 * @param config 显示配置（可以为NULL，使用默认配置）
 * @return 成功返回0，失败返回-1
 */
int display_rgb_init(void);

/**
 * @brief 从文件加载并显示RGB图像
 * @param filename RGB文件路径
 * @return 成功返回0，失败返回-1
 */
int display_rgb_from_file(const char *filename);

/**
 * @brief 拷贝数据，从内存缓冲区显示RGB图像
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