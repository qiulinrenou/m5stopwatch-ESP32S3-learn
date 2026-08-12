#ifndef WATCH_CORE_H
#define WATCH_CORE_H

#include <stdint.h>                                  // 提供更新周期类型

#include "esp_err.h"                                 // 提供 esp_err_t
#include "watch_rtc.h"                               // 提供 RTC 日期时间快照类型

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*watch_core_rtc_reader_t)(
    watch_rtc_datetime_t *datetime
);                                                   // RTC 读取回调，在 LVGL 锁外执行

typedef esp_err_t (*watch_core_battery_reader_t)(
    uint8_t *percent
);                                                   // 电量读取回调，在 LVGL 锁外执行

typedef struct {
    watch_core_rtc_reader_t rtc_reader;               // 由 main 注入的 RX8130 读取函数
    watch_core_battery_reader_t battery_reader;       // 由 main 注入的电量读取函数
    uint32_t update_period_ms;                        // 表盘真实数据更新周期
    int16_t timezone_offset_minutes;                  // RTC UTC 时间转换到本地时间的分钟偏移
} watch_core_config_t;

/**
 * @brief 启动表盘真实数据协调任务。
 *
 * 任务会在 LVGL 锁外读取 RTC 和电量，再通过 watch_lvgl_run() 同步更新界面。
 *
 * @param config 数据读取回调和更新周期。
 * @return ESP_OK 表示任务创建成功，否则返回具体错误码。
 */
esp_err_t watch_core_start(const watch_core_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
