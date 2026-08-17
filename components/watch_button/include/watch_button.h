#ifndef WATCH_BUTTON_H
#define WATCH_BUTTON_H

#include <stdint.h>                                  // 提供轮询周期和去抖周期的固定宽度类型

#include "driver/gpio.h"                             // 提供 GPIO 编号类型
#include "esp_err.h"                                 // 提供 ESP-IDF 标准错误码

/**
 * @brief 定义按键确认一次完整单击后的应用层回调。
 *
 * @param user_data 由 watch_button_start() 原样传回的应用层上下文。
 * @return 无返回值。
 * @note 回调运行在 watch_button 自己的 FreeRTOS 任务中，不得直接调用 LVGL API。
 */
typedef void (*watch_button_click_cb_t)(void *user_data);

typedef struct {
    gpio_num_t gpio_num;                              // 使用的 ESP32 GPIO 输入引脚
    uint32_t poll_period_ms;                          // GPIO 轮询周期，单位为毫秒
    uint32_t debounce_period_ms;                      // 状态稳定满该时间后确认边沿，单位为毫秒
    watch_button_click_cb_t on_click;                // 在按下后释放的完整单击事件中调用
    void *user_data;                                  // 交给 on_click 的应用层上下文
} watch_button_config_t;

/**
 * @brief 配置一个低电平按下、高电平释放的单击按键并启动轮询任务。
 *
 * @param config GPIO、轮询、去抖和单击回调配置，函数会复制该配置。
 * @return ESP_OK 表示任务已启动；参数无效、GPIO 配置失败或任务创建失败时返回错误码。
 * @note 组件只处理 GPIO 和去抖，不依赖 LVGL；应用层回调负责将 UI 操作转交给正确的锁。
 */
esp_err_t watch_button_start(const watch_button_config_t *config);

#endif
