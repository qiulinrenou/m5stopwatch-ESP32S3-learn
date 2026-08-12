#ifndef WATCH_RTC_H
#define WATCH_RTC_H

#include <stdint.h>                                  // 提供固定宽度日期时间字段

#include "driver/i2c_master.h"                       // 提供共享 I2C 总线句柄
#include "esp_err.h"                                 // 提供 esp_err_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;                                   // 完整年份，例如 2026
    uint8_t month;                                   // 月份，范围 1-12
    uint8_t day;                                     // 日期，范围 1-31
    uint8_t weekday;                                 // 星期，范围 0-6，0 表示星期日
    uint8_t hour;                                    // 小时，范围 0-23
    uint8_t minute;                                  // 分钟，范围 0-59
    uint8_t second;                                  // 秒，范围 0-59
} watch_rtc_datetime_t;

/**
 * @brief 在共享 I2C 总线上初始化 RX8130 实时时钟。
 *
 * 初始化会探测设备、启用后备电池充电，并连续读取两次时间确认时钟正在运行。
 * 本函数不会改写 RTC 中保存的日期时间；只有日期字段合法且秒值持续前进时，
 * 才会清除旧的掉电标志 VLF，并要求用户核对实际时间。
 *
 * @param i2c_bus board_power 创建的共享 I2C 主总线。
 * @return ESP_OK 表示 RTC 时间有效且正在运行；ESP_ERR_INVALID_STATE 表示 RTC
 *         存在但时间不可信；其他错误表示设备或总线初始化失败。
 */
esp_err_t watch_rtc_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief 读取并校验 RX8130 当前日期时间。
 *
 * @param datetime 输出日期时间快照。
 * @return ESP_OK 表示数据有效；ESP_ERR_INVALID_STATE 表示 VLF 已置位或字段非法。
 */
esp_err_t watch_rtc_get_datetime(watch_rtc_datetime_t *datetime);

/**
 * @brief 使用本次固件的编译时间恢复无效的 RX8130。
 *
 * 编译时间被视为 local_timezone_offset_minutes 所代表的本地时间，写入 RTC 前
 * 会转换为 UTC。该接口仅用于 RTC 首次上电或掉电后没有合法日期的恢复场景。
 *
 * @param local_timezone_offset_minutes 编译电脑本地时间相对 UTC 的分钟偏移。
 * @return ESP_OK 表示写入、读回和走时检查成功，否则返回具体错误码。
 *
 * @note 调用前必须先调用 watch_rtc_init()，并确认其返回 ESP_ERR_INVALID_STATE。
 * @note 编译时间只提供近似初始值，烧录后应与手机时间核对。
 */
esp_err_t watch_rtc_restore_from_build_time(
    int16_t local_timezone_offset_minutes
);

#ifdef __cplusplus
}
#endif

#endif
