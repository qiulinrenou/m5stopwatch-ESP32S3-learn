#include "watch_core.h"

#include <stdbool.h>                                 // 提供状态标记类型
#include <string.h>                                  // 提供结构体初始化

#include "esp_log.h"                                 // 提供数据状态日志
#include "freertos/FreeRTOS.h"                      // 提供 FreeRTOS 基础类型
#include "freertos/task.h"                          // 提供任务创建和周期延时
#include "watch_lvgl.h"                              // 提供统一的 LVGL 加锁入口
#include "watch_ui.h"                                // 提供表盘数据更新接口

#define WATCH_CORE_TASK_STACK_SIZE 4096              // 数据任务不进行大块栈分配
#define WATCH_CORE_TASK_PRIORITY   2                 // 低于显示和交互相关任务
#define WATCH_CORE_MIN_PERIOD_MS   250               // 防止配置过快造成无意义的 I2C 压力
#define WATCH_CORE_MIN_TIMEZONE_MINUTES (-12 * 60)   // 支持的最小时区 UTC-12
#define WATCH_CORE_MAX_TIMEZONE_MINUTES (14 * 60)    // 支持的最大时区 UTC+14

static const char *TAG = "WATCH_CORE";               // 当前模块日志标签
static watch_core_config_t s_config = {0};           // 保存任务长期使用的回调配置
static TaskHandle_t s_task_handle = NULL;            // 防止重复创建数据任务

/**
 * @brief 返回指定月份允许的最大日期。
 */
static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };

    if (month == 2U) {
        const bool leap =
            ((year % 4U) == 0U && (year % 100U) != 0U) ||
            ((year % 400U) == 0U);
        return leap ? 29U : 28U;
    }

    return days[month - 1U];
}

/**
 * @brief 将 RX8130 保存的 UTC 时间转换为配置的本地时间。
 */
static void apply_timezone(
    watch_rtc_datetime_t *datetime,
    int16_t offset_minutes)
{
    int32_t local_minutes =
        (int32_t)datetime->hour * 60 +
        datetime->minute +
        offset_minutes;

    while (local_minutes < 0) {                      // 跨到 UTC 日期的前一天
        local_minutes += 24 * 60;
        datetime->weekday =
            (uint8_t)((datetime->weekday + 6U) % 7U);

        if (datetime->day > 1U) {
            --datetime->day;
        } else {
            if (datetime->month > 1U) {
                --datetime->month;
            } else {
                datetime->month = 12U;
                --datetime->year;
            }
            datetime->day =
                days_in_month(datetime->year, datetime->month);
        }
    }

    while (local_minutes >= 24 * 60) {               // 跨到 UTC 日期的后一天
        local_minutes -= 24 * 60;
        datetime->weekday =
            (uint8_t)((datetime->weekday + 1U) % 7U);

        if (datetime->day <
            days_in_month(datetime->year, datetime->month)) {
            ++datetime->day;
        } else {
            datetime->day = 1U;
            if (datetime->month < 12U) {
                ++datetime->month;
            } else {
                datetime->month = 1U;
                ++datetime->year;
            }
        }
    }

    datetime->hour = (uint8_t)(local_minutes / 60);
    datetime->minute = (uint8_t)(local_minutes % 60);
}

/**
 * @brief 在 watch_lvgl_run() 的统一锁内应用一份数据快照。
 */
static void apply_ui_snapshot(lv_display_t *display, void *user_data)
{
    const watch_ui_data_t *data =                    // 快照在同步调用返回前始终有效
        (const watch_ui_data_t *)user_data;

    watch_ui_update(display, data);                  // watch_ui 只接收数据，不访问硬件
}

/**
 * @brief 周期读取真实数据并发布给主表盘。
 */
static void watch_core_task(void *user_data)
{
    (void)user_data;                                 // 当前任务使用模块私有配置

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t period =
        pdMS_TO_TICKS(s_config.update_period_ms);
    bool last_battery_valid = false;
    uint8_t last_battery_percent = 0;
    esp_err_t previous_rtc_result = ESP_ERR_INVALID_RESPONSE;

    while (true) {
        watch_ui_data_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));       // 每轮发布独立且完整的数据快照

        watch_rtc_datetime_t datetime = {0};
        const esp_err_t rtc_result =
            s_config.rtc_reader(&datetime);          // I2C 读取明确发生在 LVGL 锁外

        if (rtc_result == ESP_OK) {
            apply_timezone(                           // 原厂固件约定 RX8130 保存 UTC 时间
                &datetime,
                s_config.timezone_offset_minutes
            );
            snapshot.time_valid = true;
            snapshot.year = datetime.year;
            snapshot.month = datetime.month;
            snapshot.day = datetime.day;
            snapshot.weekday = datetime.weekday;
            snapshot.hour = datetime.hour;
            snapshot.minute = datetime.minute;
            snapshot.second = datetime.second;
        }

        if (rtc_result != previous_rtc_result) {      // 只在状态变化时记录，避免每秒刷屏
            if (rtc_result == ESP_OK) {
                ESP_LOGI(TAG, "RTC 数据恢复正常");
            } else {
                ESP_LOGW(TAG, "RTC 数据当前不可用: %s",
                         esp_err_to_name(rtc_result));
            }
            previous_rtc_result = rtc_result;
        }

        uint8_t battery_percent = 0;
        if (s_config.battery_reader(&battery_percent) == ESP_OK &&
            battery_percent <= 100U) {
            last_battery_percent = battery_percent;  // 保存最近一次有效的滤波电量
            last_battery_valid = true;
        }

        snapshot.battery_valid = last_battery_valid;
        snapshot.battery_percent = last_battery_percent;

        const esp_err_t ui_result =
            watch_lvgl_run(apply_ui_snapshot, &snapshot);
        if (ui_result != ESP_OK) {
            ESP_LOGW(TAG, "更新主表盘失败: %s", esp_err_to_name(ui_result));
        }

        vTaskDelayUntil(&last_wake_time, period);     // 使用绝对周期避免长期刷新漂移
    }
}

esp_err_t watch_core_start(const watch_core_config_t *config)
{
    if (config == NULL ||
        config->rtc_reader == NULL ||
        config->battery_reader == NULL ||
        config->update_period_ms < WATCH_CORE_MIN_PERIOD_MS ||
        config->timezone_offset_minutes < WATCH_CORE_MIN_TIMEZONE_MINUTES ||
        config->timezone_offset_minutes > WATCH_CORE_MAX_TIMEZONE_MINUTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task_handle != NULL) {                     // 当前运行时只能启动一个数据协调任务
        return ESP_ERR_INVALID_STATE;
    }

    s_config = *config;                              // 回调配置按值复制到模块私有存储

    const BaseType_t created = xTaskCreate(
        watch_core_task,
        "watch_core",
        WATCH_CORE_TASK_STACK_SIZE,
        NULL,
        WATCH_CORE_TASK_PRIORITY,
        &s_task_handle
    );

    if (created != pdPASS) {
        memset(&s_config, 0, sizeof(s_config));
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "表盘真实数据任务已启动，周期=%lums",
             (unsigned long)config->update_period_ms);
    return ESP_OK;
}
