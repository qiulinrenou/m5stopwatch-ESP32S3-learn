#ifndef KEY_H
#define KEY_H

#include "esp_err.h"                                 // 提供 ESP-IDF 标准错误码

/**
 * @brief 定义按键确认完整单击后的应用层回调类型。
 *
 * @param user_data 由 key_init() 原样传回的应用层上下文。
 * @return 无返回值。
 * @note 回调运行在 key.c 的 FreeRTOS 任务中，不能直接调用 LVGL API。
 */
typedef void (*key_click_cb_t)(void *user_data);

typedef struct {
    key_click_cb_t g1_on_click;                        // 蓝色 G1 单击后调用的应用层回调
    void *g1_user_data;                                // 蓝色 G1 回调对应的应用层上下文
    key_click_cb_t g2_on_click;                        // 黄色 G2 单击后调用的应用层回调，可为 NULL
    void *g2_user_data;                                // 黄色 G2 回调对应的应用层上下文
    key_click_cb_t g2_on_long_press;                   // 黄色 G2 长按达到阈值后调用的应用层回调，可为 NULL
    void *g2_long_press_user_data;                     // 黄色 G2 长按回调对应的应用层上下文
} key_config_t;

/**
 * @brief 初始化 G1、G2 的 GPIO 输入、去抖、单击和长按轮询任务。
 *
 * @param config 两个按键的回调配置；至少登记一个单击或长按回调。
 * @return ESP_OK 表示按键任务已启动；重复初始化、参数无效或任务创建失败时返回错误码。
 * @note 本模块只管理 GPIO1 和 GPIO2；调用者必须在回调中通过 watch_lvgl_run() 交接 UI 操作。
 */
esp_err_t key_init(const key_config_t *config);

#endif
