#ifndef WATCH_UI_H
#define WATCH_UI_H

#include <stdbool.h>                                 // 提供数据有效状态
#include <stdint.h>                                  // 提供固定宽度日期时间字段

#include "esp_lv_adapter_display.h"                  // 提供 LVGL 8/9 兼容的 lv_display_t 类型

typedef struct {
    bool time_valid;                                 // true 表示 RTC 日期时间已经通过校验
    uint16_t year;                                   // 完整年份，例如 2026
    uint8_t month;                                   // 月份，范围 1-12
    uint8_t day;                                     // 日期，范围 1-31
    uint8_t weekday;                                 // 星期，范围 0-6，0 表示星期日
    uint8_t hour;                                    // 小时，范围 0-23
    uint8_t minute;                                  // 分钟，范围 0-59
    uint8_t second;                                  // 秒，范围 0-59
    bool battery_valid;                              // true 表示电量百分比有效
    uint8_t battery_percent;                         // 经过板级滤波的电量，范围 0-100
} watch_ui_data_t;

/**
 * @brief 创建手表的初始主表盘界面。
 *
 * 本函数只负责创建 LVGL 对象，不负责初始化 LVGL adapter 或读取硬件。
 * 函数由 watch_lvgl_start() 在统一的 LVGL 锁内调用。
 *
 * @param display 已经由 watch_lvgl 注册完成的 LVGL Display。
 */
void ui_init(lv_display_t *display);

/**
 * @brief 使用一份完整数据快照刷新主表盘。
 *
 * @param display watch_lvgl 已注册的 Display。
 * @param data 已经在 LVGL 锁外读取完成的数据快照。
 *
 * @note 本函数不自行加锁，必须由 watch_lvgl_start() 或 watch_lvgl_run() 调用。
 * @note 本函数只更新 LVGL 对象，不允许执行 I2C 或其他阻塞硬件访问。
 */
void watch_ui_update(
    lv_display_t *display,
    const watch_ui_data_t *data
);

#endif
